// WebOfShadowsRecomp for the PS5 - native renderer P3, stage 1: run the game with NO
// emulated GPU, to measure the CPU-side ceiling.
//
// The native renderer (a port of Edge of Time's) replaces rexglue's emulated Xenos GPU
// entirely: like Edge of Time, the title then runs with config.graphics = nullptr and
// draws from the game's D3D calls instead of from the command stream. Stage 1 does only
// the first half: the emulated GPU is not created and nothing is drawn. What is left is
// the game's own code, so the frame rate logged is the most native rendering can ever
// reach - if this is under 60, no renderer gets 60; if it is well over, the GPU
// emulation was what held the game back.
//
// Without a GPU nothing consumes the game's PM4 ring or writes the GPU's write-backs,
// so every wait on them would block for ever. Edge of Time no-ops its waits
// (src/gpu/hooks/stubs.cpp: BlockOnFence, BlockOnSecondaryPosition,
// WaitPrimaryRingSpace, IsFencePending, BlockUntilIdle). WoS's D3D funnels all six of
// its wait loops through one spin step, which ends a wait when it returns 0:
//
//   sub_82AC0C10  spin once (wait loops in sub_82ABA260 BlockOnFence, sub_82AB9548
//                 BlockOnSecondaryPosition, sub_82AB95E0 WaitPrimaryRingSpace, sub_82ACE008,
//                 sub_82ACE208, and Swap) -> returns 0: stop waiting
//   sub_82ABA098  InsertFence -> after it, the fence write-back is set to the fence just
//                 inserted, as D3D itself does in its "GPU is gone" mode (the store it
//                 makes when device byte 10941 has bit 0x2), so anything that reads the
//                 write-back directly sees every fence passed
//   sub_82ABAC10  IsFencePending -> 0
//   sub_82AC4E48  D3DDevice_Swap -> counts the frame and returns 0 without running (as
//                 Edge of Time's Swap hook does): no VdSwap, no vblank wait
//
// The GPU's registers at 0x7FC80000 are plain memory without a GPU (the runtime maps
// 0x7F000000-0x7FFFFFFF to the 0xC0000000 heap), so D3D's ring kicks land harmlessly.
//
// Off unless wos_headless is set. When off, every hook passes straight through.
//
// SPDX-License-Identifier: MIT
// Approach from EdgeOfTimeRecomp (BSD-3-Clause, (c) 2026 Tom Clay):
// src/reeot_app.cpp (config.graphics = nullptr), src/gpu/hooks/stubs.cpp, Swap hook.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include "hooks/perf_monitor.h"

REXCVAR_DEFINE_BOOL(wos_sleep_probe, false, "WoS",
                    "Log, every 10 seconds, where the game's threads call Sleep from and how "
                    "often: what the main thread's idle time is spent waiting for");
REXCVAR_DEFINE_BOOL(wos_headless, false, "WoS",
                    "Native renderer P3 stage 1: run with no emulated GPU and draw nothing, "
                    "to measure how fast the game's own code runs");

namespace wos::threads {
void NoteSwap();  // wos_ps5_threads.cpp: long frames
}

namespace wos::native_renderer {
bool Enabled();  // wos_ps5_native_renderer.cpp
}
extern "C" void wos_eot_Swap(PPCContext &ctx, uint8_t *base);
namespace wos::capture {
void OnSwap();
void Call(const char *name, const PPCContext &ctx, uint8_t *base, bool draw);
}  // the native renderer's present

namespace wos::headless {
namespace {

std::atomic<int> g_state{-1};  // -1 unknown, 0 off, 1 on
std::atomic<uint64_t> g_spins{0}, g_fences{0}, g_swaps{0}, g_pending{0};

inline uint32_t Load32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  __builtin_memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t *>(base), va), 4);
  return __builtin_bswap32(v);
}
inline void Store32(uint8_t *base, uint32_t va, uint32_t value) {
  value = __builtin_bswap32(value);
  __builtin_memcpy(rex::memory::GuestPtr(base, va), &value, 4);  // +4 KiB above 0xE0000000 on the PS5
}

constexpr uint32_t kFenceWriteback = 10896;  // device: pointer to the GPU's write-backs
constexpr uint32_t kNextFence = 10908;       // device: the next fence to insert

}  // namespace

// No emulated GPU: headless, or the native renderer (which needs the same stubs: its
// game-side D3D still waits on fences no GPU will ever pass)
bool Enabled() {
  int s = g_state.load(std::memory_order_relaxed);
  if (s < 0) {
    s = (REXCVAR_GET(wos_headless) || native_renderer::Enabled()) ? 1 : 0;
    g_state.store(s, std::memory_order_relaxed);
  }
  return s == 1;
}

void Report() {
  REXLOG_INFO("[WoS-HEADLESS] swaps {} | fences {} | waits ended {} | fence queries {}",
              g_swaps.load(), g_fences.load(), g_spins.load(), g_pending.load());
}

}  // namespace wos::headless

extern "C" void __imp__sub_82AC0C10(PPCContext &ctx, uint8_t *base);  // spin once
extern "C" void __imp__sub_82ABA098(PPCContext &ctx, uint8_t *base);  // InsertFence
extern "C" void __imp__sub_82ABAC10(PPCContext &ctx, uint8_t *base);  // IsFencePending
extern "C" void __imp__sub_82AC4E48(PPCContext &ctx, uint8_t *base);  // Swap

namespace wos::headless {
void PollFences(uint8_t *base);
}

// The spin step of every D3D wait: 0 ends the wait.
extern "C" void sub_82AC0C10(PPCContext &ctx, uint8_t *base) {
  if (!wos::headless::Enabled()) {
    __imp__sub_82AC0C10(ctx, base);
    return;
  }
  wos::headless::g_spins.fetch_add(1, std::memory_order_relaxed);
  wos::headless::PollFences(base);  // fix43: waits that read the write-back themselves
  ctx.r3.u64 = 0;
}

// fix43: honest fences with the native renderer. A fence passes once EoT's render thread has
// replayed every command captured before it (texture uploads read guest memory during the
// replay; vertex and index data are copied at capture). fix42 and before passed every fence
// at once: the game reused memory the render thread had yet to read - one-frame wrong
// textures and broken meshes. wos_honest_fences = false restores the old behaviour.
extern "C" uint64_t wos_rt_committed();
extern "C" uint64_t wos_rt_executed();
extern "C" void wos_rt_publish();
extern "C" void wos_rt_wait(uint64_t seq);
extern "C" bool wos_rt_active();
REXCVAR_DEFINE_BOOL(wos_honest_fences, true, "WoS",
                    "Native renderer: a D3D fence passes when the render thread has replayed "
                    "the commands before it (false: every fence passes at once)");
namespace wos::headless {
namespace {
struct FenceTrack {
  std::mutex m;
  std::vector<std::pair<uint32_t, uint64_t>> pending;  // fence value, render seq
  uint32_t device = 0;
  uint64_t waits = 0, waited_us = 0;
};
FenceTrack &fences() {
  static FenceTrack f;
  return f;
}
bool Honest() {
  static const bool on = REXCVAR_GET(wos_honest_fences);
  return on && native_renderer::Enabled() && wos_rt_active();
}
// The write-back to the newest fence whose commands the render thread has replayed.
void UpdateFences(uint8_t *base) {
  FenceTrack &f = fences();
  std::lock_guard lock(f.m);
  if (!f.device || f.pending.empty())
    return;
  const uint64_t done = wos_rt_executed();
  size_t n = 0;
  uint32_t passed = 0;
  bool any = false;
  while (n < f.pending.size() && f.pending[n].second <= done) {
    passed = f.pending[n].first;
    any = true;
    ++n;
  }
  if (!any)
    return;
  f.pending.erase(f.pending.begin(), f.pending.begin() + n);
  const uint32_t writeback = Load32(base, f.device + kFenceWriteback);
  if (writeback)
    Store32(base, writeback, passed);
}
// The render sequence a fence waits for (0: passed or unknown).
uint64_t FenceSeq(uint32_t fence) {
  FenceTrack &f = fences();
  std::lock_guard lock(f.m);
  for (const auto &[value, seq] : f.pending)
    if (int32_t(value - fence) >= 0)
      return seq;
  return 0;
}
}  // namespace
void PollFences(uint8_t *base) {
  if (Honest())
    UpdateFences(base);
}
}  // namespace wos::headless

// InsertFence(device = r3, command stream = r4): after it, mark the fence passed (or, with
// honest fences, once the render thread gets there).
extern "C" void sub_82ABA098(PPCContext &ctx, uint8_t *base) {
  const uint32_t device = ctx.r3.u32;
  __imp__sub_82ABA098(ctx, base);
  if (!wos::headless::Enabled() || !device) return;
  using namespace wos::headless;
  const uint32_t writeback = Load32(base, device + kFenceWriteback);
  const uint32_t fence = Load32(base, device + kNextFence) - 2;
  if (Honest()) {
    {
      FenceTrack &f = fences();
      std::lock_guard lock(f.m);
      f.device = device;
      f.pending.emplace_back(fence, wos_rt_committed());
      if (f.pending.size() > 4096)  // never left to grow without bound
        f.pending.erase(f.pending.begin(), f.pending.begin() + 2048);
    }
    UpdateFences(base);
  } else if (writeback) {
    Store32(base, writeback, fence);
  }
  if (g_fences.fetch_add(1, std::memory_order_relaxed) == 0) {
    REXLOG_INFO("[WoS-HEADLESS] first fence: device {:08X}, write-back {:08X}", device, writeback);
  }
}

// IsFencePending(fence = r3; the device is D3D's global) -> never pending (honest fences: as D3D computes
// it, against the write-back the render thread's progress moves).
extern "C" void sub_82ABAC10(PPCContext &ctx, uint8_t *base) {
  if (!wos::headless::Enabled()) {
    __imp__sub_82ABAC10(ctx, base);
    return;
  }
  using namespace wos::headless;
  g_pending.fetch_add(1, std::memory_order_relaxed);
  if (Honest()) {
    UpdateFences(base);
    if (FenceSeq(ctx.r3.u32))
      wos_rt_publish();  // a waiter polls: make sure the render thread has the commands
    __imp__sub_82ABAC10(ctx, base);
    return;
  }
  ctx.r3.u64 = 0;
}

// BlockOnFence(device = r3, fence = r4): honest fences wait for the render thread here (its
// own spin loop ends at once: sub_82AC0C10 returns 0).
extern "C" void __imp__sub_82ABA260(PPCContext &ctx, uint8_t *base);
extern "C" void sub_82ABA260(PPCContext &ctx, uint8_t *base) {
  using namespace wos::headless;
  if (Enabled() && Honest() && ctx.r3.u32) {
    if (const uint64_t seq = FenceSeq(ctx.r4.u32)) {
      const auto t0 = std::chrono::steady_clock::now();
      wos_rt_publish();
      wos_rt_wait(seq);
      UpdateFences(base);
      FenceTrack &f = fences();
      const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - t0).count();
      std::lock_guard lock(f.m);
      ++f.waits;
      f.waited_us += us;
      if (f.waits <= 3 || f.waits % 2000 == 0)
        REXLOG_INFO("[WoS-FENCE] BlockOnFence waited for the render thread {} us ({} waits, {} ms "
                    "in all)", us, f.waits, f.waited_us / 1000);
    }
  }
  __imp__sub_82ABA260(ctx, base);
}

// sub_82ABAD58(device = r3): kicks off the ring buffer with a packet the GPU answers by
// clearing device+11008 (the count of kick-offs the GPU still owes an interrupt for),
// then spins on that word - the pause menu and the tutorial/character videos reach it
// (the native run's freeze: the main thread's every sample at 0x82ABADB0). No GPU here:
// the word is cleared and the wait skipped.
extern "C" void __imp__sub_82ABAD58(PPCContext &ctx, uint8_t *base);
extern "C" void sub_82ABAD58(PPCContext &ctx, uint8_t *base) {
  if (!wos::headless::Enabled()) {
    __imp__sub_82ABAD58(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  if (device) wos::headless::Store32(base, device + 11008, 0);
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) == 0)
    REXLOG_INFO("[WoS-HEADLESS] GPU idle wait (sub_82ABAD58) skipped, device {:08X}", device);
}

// D3DDevice_Swap(device = r3, front buffer = r4, parameters = r5): count, show nothing.
// --- Vblank interrupts (native renderer) ----------------------------------------------
//
// Without a GPU the kernel ignores VdSetGraphicsInterruptCallback, so the game's graphics
// interrupt (sub_82AB9840) never runs - and with it neither does its vblank handler,
// sub_82AC46C8 (device): the device's vblank counter (+16548) never moved and the game's
// vblank callback (+16544, called with {vblanks, flips, 0}) never ran. The game times
// effects from it: the eye adaptation's rate was 0 every frame, the adapted luminance sat
// at its 0.001 floor and the tone map ran at full exposure (the white and coloured
// washes). Here each Swap runs the handler once for every 59.94 Hz vblank of real time
// since the previous one (1 to 4), on the game's thread, as the interrupt would have.
// (Its flip path stays idle: this Swap queues no flips, and the GPU registers it would
// write are plain memory here.)
extern "C" void sub_82AC46C8(PPCContext &ctx, uint8_t *base);  // the device's vblank handler
namespace wos::headless {
void EmulateVblanks(PPCContext &ctx, uint8_t *base, uint32_t device) {
  using clock = std::chrono::steady_clock;
  static clock::time_point last{};
  static double owed = 0.0;
  static std::atomic<uint64_t> total{0};
  if (!device) return;
  const clock::time_point now = clock::now();
  int n = 1;
  if (last != clock::time_point{}) {
    // WoS port (fix39): at the rate the kernel reports to the game (VdQueryVideoMode, from
    // video_mode_refresh_rate), not a fixed 59.94: the game paces on two vblanks a frame, so
    // with 120 configured it was told 120 Hz yet counted 59.94 Hz ticks - capped at 30 fps
    static const double hz = std::clamp(REXCVAR_GET(video_mode_refresh_rate), 24.0, 240.0) *
                             (59.94006 / 60.0);
    owed += std::chrono::duration<double, std::milli>(now - last).count() / (1000.0 / hz);
    n = std::clamp(static_cast<int>(owed), 0, 8);
    owed = std::min(owed - n, 8.0);
  }
  last = now;
  if (n <= 0) return;
  const PPCContext saved = ctx;
  for (int i = 0; i < n; ++i) {
    ctx.r3.u64 = device;
    sub_82AC46C8(ctx, base);
    ctx = saved;
  }
  const uint64_t t = total.fetch_add(n, std::memory_order_relaxed) + n;
  if (t <= 4 || t % 7200 < uint64_t(n))
    REXLOG_INFO("[WoS-HEADLESS] vblank {}: device {:08X} counter {} callback {:08X}", t, device,
                Load32(base, device + 16548), Load32(base, device + 16544));
}
}  // namespace wos::headless

extern "C" void sub_82AC4E48(PPCContext &ctx, uint8_t *base) {
  if (!wos::headless::Enabled()) {
    __imp__sub_82AC4E48(ctx, base);
    return;
  }
  wos::perf::RecordSwap();
  wos::threads::NoteSwap();
  wos::capture::OnSwap();
  if (wos::native_renderer::Enabled()) wos::capture::Call("Swap", ctx, base, false);
  const uint64_t n = wos::headless::g_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || n % 600 == 0) wos::headless::Report();
  if (wos::native_renderer::Enabled()) {
    wos::headless::PollFences(base);
    wos::headless::EmulateVblanks(ctx, base, ctx.r3.u32);
    wos_eot_Swap(ctx, base);  // presents the frame, returns 0
    return;
  }
  ctx.r3.u64 = 0;
}

// --- Sleep probe (wos_sleep_probe) ---------------------------------------------------
//
// Headless gameplay ran 49-56 fps with the main game thread ("Game Master") asleep ~40%
// of the time, in some 2,100 sleeps a second, while no thread was saturated: the game
// is waiting on something in a sleep loop. Sleep (sub_82B13248, tail-calls SleepEx) and
// its alertable twin (sub_82B13210) are the only ways the game sleeps; this counts each
// call by thread, caller (ctx.lr) and the milliseconds asked, and logs the top sites.

namespace wos::headless {
namespace {

std::atomic<int> g_probe{-1};
std::mutex g_probe_mutex;
struct SleepKey {
  uint32_t thread, caller, ms;
  bool operator<(const SleepKey &o) const {
    return std::tie(thread, caller, ms) < std::tie(o.thread, o.caller, o.ms);
  }
};
std::map<SleepKey, uint64_t> g_sleeps;
std::map<uint32_t, std::string> g_thread_names;
std::chrono::steady_clock::time_point g_probe_last{};

bool ProbeEnabled() {
  int s = g_probe.load(std::memory_order_relaxed);
  if (s < 0) {
    s = REXCVAR_GET(wos_sleep_probe) ? 1 : 0;
    g_probe.store(s, std::memory_order_relaxed);
  }
  return s == 1;
}

void NoteSleep(uint32_t caller, uint32_t ms) {
  auto *thread = rex::system::XThread::GetCurrentThread();
  const uint32_t id = thread ? thread->handle() : 0;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(g_probe_mutex);
  ++g_sleeps[{id, caller, ms}];
  if (thread && !g_thread_names.count(id)) g_thread_names[id] = thread->thread_name();
  if (g_probe_last.time_since_epoch().count() == 0) g_probe_last = now;
  if (now - g_probe_last < std::chrono::seconds(10)) return;
  g_probe_last = now;
  std::vector<std::pair<uint64_t, SleepKey>> top;
  for (const auto &[key, count] : g_sleeps) top.push_back({count, key});
  std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
  for (size_t i = 0; i < top.size() && i < 10; ++i) {
    const auto &k = top[i].second;
    REXLOG_INFO("[WoS-SLEEP] {:>7} x Sleep({}) from {:08X} on '{}' ({:08X})", top[i].first, k.ms,
                k.caller, g_thread_names[k.thread], k.thread);
  }
  g_sleeps.clear();
}

}  // namespace
}  // namespace wos::headless

extern "C" void __imp__sub_82B13248(PPCContext &ctx, uint8_t *base);  // Sleep(ms)
extern "C" void __imp__sub_82B13210(PPCContext &ctx, uint8_t *base);  // SleepEx(ms, alertable)

extern "C" void sub_82B13248(PPCContext &ctx, uint8_t *base) {
  if (wos::headless::Enabled()) wos::headless::PollFences(base);
  if (wos::headless::ProbeEnabled()) wos::headless::NoteSleep(uint32_t(ctx.lr), ctx.r3.u32);
  __imp__sub_82B13248(ctx, base);
}
extern "C" void sub_82B13210(PPCContext &ctx, uint8_t *base) {
  if (wos::headless::Enabled()) wos::headless::PollFences(base);
  if (wos::headless::ProbeEnabled()) wos::headless::NoteSleep(uint32_t(ctx.lr), ctx.r3.u32);
  __imp__sub_82B13210(ctx, base);
}
