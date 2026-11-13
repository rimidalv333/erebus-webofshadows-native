// WebOfShadowsRecomp for the PS5 - where the game's threads spend their time.
//
// With the emulated GPU's command processor waiting for the game 60-70% of the
// time, the game's own threads hold the frame rate. The title links the kernel
// calls a thread blocks or spins in with --wrap (wos/wos_game.cmake), and each
// thread adds up its time in them by kind, timed with the TSC. Every ten
// seconds the frame rate log (wos_ps5_app.cpp) prints the threads that were
// busy: the share of the window each spent running its own code (what is left),
// waiting on objects, sleeping or yielding, entering critical sections, taking
// spin locks, reading files and in VdSwap.
//
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <vector>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <x86intrin.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <ucontext.h>
#if defined(__PROSPERO__)
#include <pthread_np.h>
#include <sys/cpuset.h>
#include <ps5platform/libc.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/ui/startup_prospero.h>

// fix114: a guarded guest-memory read (wos_ps5_native_renderer.cpp)
extern "C" bool wos_guest_be32(uint32_t va, uint32_t* out);

// 0028: defined in wos_ps5_app.cpp
REXCVAR_DECLARE(bool, wos_diagnostics);
REXCVAR_DECLARE(bool, wos_profile);
// WoS port (fix114)
REXCVAR_DEFINE_BOOL(wos_slow_frame_profile, true, "WoS",
                    "fix114: with wos_profile/wos_diagnostics, log what the main and draw threads do in "
                    "slow (>= 18.5 ms) vs fast frames ([WoS-SLOW])");
REXCVAR_DEFINE_BOOL(wos_spin_sleep0_draw, false, "WoS",
                    "fix114: the draw thread (JQ worker 1, own CPU) spins instead of the PS5's ~160 us "
                    "Sleep(0) yield");
REXCVAR_DEFINE_BOOL(wos_main_sleep0_yield, true, "WoS",
                    "fix115: the main thread's Sleep(0) is a real yield (as for normal-priority threads) "
                    "instead of the runtime's 100 us sleep for below-normal priority (~200 us on the PS5)");
REXCVAR_DEFINE_BOOL(wos_spin_sleep0_main, false, "WoS",
                    "fix114: the main thread (own CPU) spins instead of the PS5's ~160 us Sleep(0) yield");

extern "C" uint64_t sceKernelGetTscFrequency(void);

namespace wos::threads {
namespace {

enum Kind { kWait, kSleep, kCriticalSection, kSpinLock, kFile, kSwap, kKindCount };
constexpr const char* kKindNames[kKindCount] = {"wait", "sleep", "critsec", "spin", "file", "swap"};
constexpr int kMaxThreads = 128;

struct Slot {
  std::atomic<uint64_t> ticks[kKindCount];
  std::atomic<uint64_t> calls[kKindCount];
  char name[48];
  uint32_t handle;
  // At the last report
  uint64_t reported_ticks[kKindCount];
  uint64_t reported_calls[kKindCount];
  uint64_t reported_at;
  // The thread's guest context (its link register is sampled, below)
  std::atomic<const PPCContext*> context{nullptr};
  // The host thread (the emulated GPU's command processor is sampled by its
  // instruction pointer, below)
  std::atomic<bool> has_pthread{false};
  pthread_t pthread{};
};

Slot g_slots[kMaxThreads];
std::atomic<int> g_slot_count{0};
thread_local Slot* t_slot = nullptr;
thread_local bool t_no_slot = false;

Slot* CurrentSlot() {
  if (t_slot || t_no_slot) {
    return t_slot;
  }
  const int index = g_slot_count.fetch_add(1);
  if (index >= kMaxThreads) {
    t_no_slot = true;
    return nullptr;
  }
  Slot& slot = g_slots[index];
  if (auto* thread = rex::system::XThread::GetCurrentThread()) {
    std::snprintf(slot.name, sizeof slot.name, "%s", thread->thread_name().c_str());
    slot.handle = thread->handle();
  } else {
    std::snprintf(slot.name, sizeof slot.name, "host thread");
  }
  slot.reported_at = __rdtsc();
  slot.pthread = pthread_self();
  slot.has_pthread.store(true, std::memory_order_release);
  t_slot = &slot;
  return t_slot;
}

}  // namespace

// fix46: the calling thread's guest context (set at its first wrapped kernel call), for the
// capture's per-draw call stacks (blob 11)
const PPCContext* CurrentContext() {
  Slot* slot = CurrentSlot();
  return slot ? slot->context.load(std::memory_order_relaxed) : nullptr;
}

// fix46b: the calling guest thread's stack, [limit, base) (KTHREAD +0x60 / +0x5C) - the
// call-stack walk reads only inside it (fix46 read one word past the base and crashed)
bool CurrentStackBounds(uint32_t* limit, uint32_t* base) {
  auto* thread = rex::system::XThread::GetCurrentThread();
  if (!thread || !thread->is_guest_thread() || !thread->guest_object()) return false;
  const auto* kthread = thread->memory()->TranslateVirtual<const rex::system::X_KTHREAD*>(
      thread->guest_object());
  if (!kthread) return false;
  *base = kthread->stack_base;
  *limit = kthread->stack_limit;
  return *limit && *base > *limit && *base - *limit <= 0x1000000;
}

extern "C" bool wos_lean_stream_on();  // wos_ps5_native_renderer.cpp (fix104)
void Account(int kind, uint64_t ticks, const PPCContext* context) {
  if (Slot* slot = CurrentSlot()) {
    if (context && !slot->context.load(std::memory_order_relaxed)) {
      slot->context.store(context, std::memory_order_relaxed);
    }
    // fix104: each slot is written by its own thread only - plain load + store, not a locked
    // add (two per kernel wait / critical section, thousands a frame)
    static const bool lean = wos_lean_stream_on();
    if (lean) {
      slot->ticks[kind].store(slot->ticks[kind].load(std::memory_order_relaxed) + ticks,
                              std::memory_order_relaxed);
      slot->calls[kind].store(slot->calls[kind].load(std::memory_order_relaxed) + 1,
                              std::memory_order_relaxed);
    } else {
      slot->ticks[kind].fetch_add(ticks, std::memory_order_relaxed);
      slot->calls[kind].fetch_add(1, std::memory_order_relaxed);
    }
  }
}

// A sampling profile of the busiest guest threads: every 200 us a host thread
// reads each one's link register, which the recompiled code sets before every
// call (ctx.lr = the return address). The most frequent values are the calls
// those threads spend their time under: where the main thread's 95% goes. Only
// the main thread and the job queue workers are sampled.
namespace {
std::mutex g_sample_lock;
std::unordered_map<uint32_t, uint32_t> g_samples[kMaxThreads];
uint32_t g_sample_total[kMaxThreads];

// WoS port (fix114): the slow-frame profile. The main thread's and the draw thread's (JQ worker
// 1) samples, each with two guest callers, are held per frame and filed at the swap under
// "slow" (>= 18.5 ms) or "fast" frames; also per frame: their time in each kind of wait and
// their Sleep(0) / timed sleep calls. Report() prints what slow frames spend more on.
constexpr int kProfThreads = 2;  // 0 Main XThread, 1 JQ worker 1 (the draw thread)
constexpr const char* kProfNames[kProfThreads] = {"main", "draw"};
int ProfIndex(const char* name) {
  if (std::strncmp(name, "Main XThread", 12) == 0) return 0;
  if (std::strncmp(name, "JQ worker 1", 11) == 0) return 1;
  return -1;
}
struct ProfKey {
  uint32_t a = 0, b = 0, c = 0;
  bool operator==(const ProfKey& o) const { return a == o.a && b == o.b && c == o.c; }
};
struct ProfKeyHash {
  size_t operator()(const ProfKey& k) const {
    return size_t((uint64_t(k.a) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(k.b) << 21) ^ k.c);
  }
};
struct ProfSample {
  uint8_t thread;
  ProfKey key;
};
std::vector<ProfSample> g_frame_samples;  // this frame's (under g_sample_lock)
std::unordered_map<ProfKey, uint32_t, ProfKeyHash> g_prof[2][kProfThreads];  // [slow][thread]
uint64_t g_prof_samples[2][kProfThreads] = {};
uint32_t g_prof_frames[2] = {};
double g_prof_ms[2] = {};
double g_prof_kind_ms[2][kProfThreads][kKindCount] = {};
uint64_t g_prof_kind_last[kProfThreads][kKindCount] = {};
// Sleep calls of the two threads (KeDelayExecutionThread): zero-length and timed
std::atomic<uint64_t> g_sl0_calls[kProfThreads], g_sl0_ticks[kProfThreads];
std::atomic<uint64_t> g_slt_calls[kProfThreads], g_slt_ticks[kProfThreads], g_slt_req_us[kProfThreads];
uint64_t g_sl_last[kProfThreads][5] = {};
double g_prof_sleep[2][kProfThreads][5] = {};  // per category: sleep0 calls, ms; timed calls, ms, requested ms

ProfKey ProfStack(const PPCContext* context, uint32_t lr) {
  ProfKey k;
  k.a = lr;
  uint32_t sp = uint32_t(*reinterpret_cast<const volatile uint64_t*>(&context->r1));
  for (int d = 0; d < 2; ++d) {
    uint32_t next = 0, ret = 0;
    if (!sp || !wos_guest_be32(sp, &next)) break;
    if (next <= sp || next - sp > 0x100000 || !wos_guest_be32(next - 8, &ret)) break;
    (d == 0 ? k.b : k.c) = ret;
    sp = next;
  }
  return k;
}

bool Sampled(const Slot& slot) {
  return std::strncmp(slot.name, "Main", 4) == 0 || std::strncmp(slot.name, "JQ worker", 9) == 0 ||
         std::strncmp(slot.name, "XThread", 7) == 0;
}

// The host profile: every 400 us the sampler signals the emulated GPU's
// command processor thread (SIGPROF), whose handler keeps the instruction
// pointer it interrupted. Report() counts them up and writes all of them, as
// offsets from wos_profile_anchor, to /app0/wos-profile.txt;
// wos/tools/profile-report.py names the functions from the linked title
// (build/ps5/link/llvm-pie.elf).
constexpr uint32_t kRingSize = 1u << 15;
constexpr int kDepth = 6;  // the instruction pointer, the return address on the stack, 4 frames
struct Stack {
  uint64_t at[kDepth];
};
Stack g_ring[kRingSize];
std::atomic<uint32_t> g_ring_head{0};
uint32_t g_ring_tail = 0;
struct StackHash {
  size_t operator()(const Stack& s) const {
    uint64_t h = 0;
    for (uint64_t v : s.at) h = (h ^ v) * 0x100000001B3ull;
    return size_t(h);
  }
};
struct StackEq {
  bool operator()(const Stack& a, const Stack& b) const {
    return std::memcmp(a.at, b.at, sizeof a.at) == 0;
  }
};
std::unordered_map<Stack, uint32_t, StackHash, StackEq> g_profile;
uint64_t g_profile_total = 0;

// The interrupted thread's instruction pointer, the word at its stack pointer
// (the return address when it was stopped in a leaf such as a system call
// stub) and the return addresses of up to four frames along the frame
// pointers (the title is built with them), each read only from within the
// thread's stack above the stack pointer.
void ProfileSignal(int, siginfo_t*, void* context) {
  const auto& machine = static_cast<ucontext_t*>(context)->uc_mcontext;
  Stack stack{};
  stack.at[0] = uint64_t(machine.mc_rip);
  const uint64_t rsp = uint64_t(machine.mc_rsp);
  constexpr uint64_t kStackSpan = 8ull << 20;
  if (rsp && !(rsp & 7)) {
    stack.at[1] = *reinterpret_cast<const uint64_t*>(rsp);
  }
  uint64_t rbp = uint64_t(machine.mc_rbp);
  for (int i = 2; i < kDepth; ++i) {
    if (rbp < rsp || rbp - rsp > kStackSpan || (rbp & 7)) {
      break;
    }
    const uint64_t* frame = reinterpret_cast<const uint64_t*>(rbp);
    stack.at[i] = frame[1];
    const uint64_t next = frame[0];
    if (next <= rbp) {
      break;
    }
    rbp = next;
  }
  const uint32_t at = g_ring_head.fetch_add(1, std::memory_order_relaxed);
  g_ring[at & (kRingSize - 1)] = stack;
}

bool HostSampled(const Slot& slot) {
  return std::strncmp(slot.name, "GPU Commands", 12) == 0;
}

void SampleLoop() {
  for (uint64_t tick = 0;; ++tick) {
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    const int count = std::min(g_slot_count.load(), kMaxThreads);
    if (tick & 1) {
      for (int i = 0; i < count; ++i) {
        Slot& slot = g_slots[i];
        if (HostSampled(slot) && slot.has_pthread.load(std::memory_order_acquire)) {
          pthread_kill(slot.pthread, SIGPROF);
        }
      }
    }
    std::lock_guard lock(g_sample_lock);
    for (int i = 0; i < count; ++i) {
      const Slot& slot = g_slots[i];
      const PPCContext* context = slot.context.load(std::memory_order_relaxed);
      if (!context || !Sampled(slot)) {
        continue;
      }
      const uint32_t lr = uint32_t(*reinterpret_cast<const volatile uint64_t*>(&context->lr));
      ++g_samples[i][lr];
      ++g_sample_total[i];
      static const bool slow_profile = REXCVAR_GET(wos_slow_frame_profile);
      if (slow_profile && g_frame_samples.size() < 8192) {
        const int t = ProfIndex(slot.name);
        if (t >= 0)
          g_frame_samples.push_back({uint8_t(t), ProfStack(context, lr)});
      }
    }
  }
}
}  // namespace

}  // namespace wos::threads

extern "C" __attribute__((noinline, used)) void wos_profile_anchor() {
  asm volatile("");
}

namespace wos::threads {
namespace {
void WriteProfile() {
  const uint32_t head = g_ring_head.load(std::memory_order_relaxed);
  uint32_t tail = g_ring_tail;
  if (head - tail > kRingSize) {
    tail = head - kRingSize;
  }
  const uint64_t anchor = reinterpret_cast<uint64_t>(&wos_profile_anchor);
  for (; tail != head; ++tail) {
    Stack stack = g_ring[tail & (kRingSize - 1)];
    for (uint64_t& v : stack.at) {
      v = v ? v - anchor : 0;
    }
    ++g_profile[stack];
    ++g_profile_total;
  }
  g_ring_tail = head;
  if (!g_profile_total) {
    return;
  }
  std::vector<std::pair<Stack, uint32_t>> all(g_profile.begin(), g_profile.end());
  std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.second > b.second; });
  if (FILE* f = std::fopen("/app0/wos-profile.txt", "w")) {
    std::fprintf(f,
                 "# wos profile 2: GPU Commands, %llu samples; count, then the instruction "
                 "pointer, the word at the stack pointer and 4 frames' return addresses, as "
                 "offsets from wos_profile_anchor (0: none)\n",
                 (unsigned long long)g_profile_total);
    for (const auto& [stack, samples] : all) {
      std::fprintf(f, "%u", samples);
      for (uint64_t v : stack.at) {
        std::fprintf(f, " %llx", (unsigned long long)v);
      }
      std::fprintf(f, "\n");
    }
    std::fclose(f);
  }
}
}  // namespace

void StartSampler() {
  static std::once_flag once;
  std::call_once(once, [] {
    struct sigaction action {};
    action.sa_sigaction = ProfileSignal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF, &action, nullptr);
    std::thread(SampleLoop).detach();
  });
}

// Long frames: each call of VdSwap (the game handing a frame to the GPU) is
// timed, and a frame of 45 ms or more (a 30 fps frame is 33 ms) is logged with
// what happened during it - pipelines made and shaders translated on the GPU
// command processor, textures loaded (rexglue patch 0019), the game's file
// reads - so the hitches that make 30 fps feel uneven can be told apart. The
// window's report counts them.
}  // namespace wos::threads
extern "C" void rex_gpu_hitch_read(uint64_t* out);
namespace wos::threads {
namespace {
struct HitchSnapshot {
  uint64_t gpu[5];
  uint64_t file_ticks, file_calls;
};
HitchSnapshot TakeHitchSnapshot() {
  HitchSnapshot snapshot{};
  rex_gpu_hitch_read(snapshot.gpu);
  const int count = std::min(g_slot_count.load(), kMaxThreads);
  for (int i = 0; i < count; ++i) {
    snapshot.file_ticks += g_slots[i].ticks[kFile].load(std::memory_order_relaxed);
    snapshot.file_calls += g_slots[i].calls[kFile].load(std::memory_order_relaxed);
  }
  return snapshot;
}
std::mutex g_hitch_lock;
uint64_t g_last_swap = 0;
HitchSnapshot g_last_snapshot{};
uint32_t g_hitches_45 = 0, g_hitches_60 = 0, g_hitches_90 = 0, g_hitches_logged = 0;
double g_worst_ms = 0;
}  // namespace

// --- fix46: CPU layout ------------------------------------------------------------------
//
// Nothing was pinned on the PS5: ignore_thread_affinities leaves the game's threads
// floating (their "(CPU n)" names are labels), and EoT's PinThreadToPhysicalCore had no
// FreeBSD branch, so its render thread floated too. Gameplay is CPU-bound on three
// threads: the game's main thread, JQ worker 1 (the world renderer: every D3D call, our
// hooks and the register mirror run on it) and EoT's render thread. As MGR/Crackdown do
// for their GPU thread, each gets a core of its own (its twin CPU left empty) and every
// other game thread is kept on the remaining CPUs. Built from the title's own affinity
// mask (CPUs 0-12 on the console); wos_cpu_layout = false turns it off.
REXCVAR_DEFINE_BOOL(wos_cpu_layout, true, "WoS",
                    "Give the render thread, the game's main thread and its world-render "
                    "worker (JQ worker 1) a core each; the other game threads share the rest");
REXCVAR_DEFINE_BOOL(wos_audio_cpu, true, "WoS",
                    "fix102: the game's Audio Worker gets a CPU of its own (the render core's "
                    "twin) - the main menu's crackling, slowed-down music");
namespace {
struct CpuLayout {
  bool ok = false;
  int render = -1, main = -1, d3d = -1;
  uint64_t others = 0, allowed = 0;
};

#if defined(__PROSPERO__)
bool SetCpuMask(pthread_t thread, uint64_t mask) {
  cpuset_t set;
  CPU_ZERO(&set);
  for (int i = 0; i < 64 && i < CPU_SETSIZE; ++i)
    if (mask & (uint64_t(1) << i)) CPU_SET(i, &set);
  return ps5_pthread_setaffinity_np(thread, sizeof set, &set) == 0;
}
#endif

const CpuLayout& GetCpuLayout() {
  static const CpuLayout layout = [] {
    CpuLayout l;
#if defined(__PROSPERO__)
    if (!REXCVAR_GET(wos_cpu_layout)) return l;
    cpuset_t set;
    CPU_ZERO(&set);
    if (ps5_pthread_getaffinity_np(pthread_self(), sizeof set, &set) != 0) return l;
    for (int i = 0; i < 64 && i < CPU_SETSIZE; ++i)
      if (CPU_ISSET(i, &set)) l.allowed |= uint64_t(1) << i;
    // whole cores (both SMT halves allowed), lowest first
    int pairs[3], found = 0;
    for (int c = 0; c + 1 < 64 && found < 3; c += 2)
      if ((l.allowed >> c & 1) && (l.allowed >> (c + 1) & 1)) pairs[found++] = c;
    if (found < 3) return l;
    l.render = pairs[0];
    l.main = pairs[1];
    l.d3d = pairs[2];
    l.others = l.allowed;
    for (int k = 0; k < 3; ++k) l.others &= ~(uint64_t(3) << pairs[k]);
    l.ok = __builtin_popcountll(l.others) >= 3;
#endif
    return l;
  }();
  return layout;
}

bool NameIs(const char* name, const char* prefix) {
  const size_t n = std::strlen(prefix);
  return std::strncmp(name, prefix, n) == 0 && (name[n] == '\0' || name[n] == ' ' || name[n] == '(');
}

uint64_t g_layout_applied[kMaxThreads] = {};
uint64_t g_layout_last = 0;
}  // namespace

// The CPU the EoT render thread takes (-1: no layout) - called from EoT (device.cpp)
extern "C" int wos_cpu_layout_render_cpu() {
  const CpuLayout& l = GetCpuLayout();
  return l.ok ? l.render : -1;
}

// Every ~5 s from the swap: threads seen since are put where the layout wants them.
void ApplyCpuLayout() {
#if defined(__PROSPERO__)
  const CpuLayout& l = GetCpuLayout();
  if (!l.ok) return;
  const uint64_t now = __rdtsc();
  static const uint64_t interval = sceKernelGetTscFrequency() * 5;
  if (g_layout_last && now - g_layout_last < interval) return;
  const bool first = g_layout_last == 0;
  g_layout_last = now;
  int moved = 0, mains = 0, d3ds = 0, audios = 0;
  const int count = std::min(g_slot_count.load(), kMaxThreads);
  for (int i = 0; i < count; ++i) {
    Slot& slot = g_slots[i];
    if (!slot.has_pthread.load(std::memory_order_acquire)) continue;
    if (std::strncmp(slot.name, "host thread", 11) == 0) continue;  // not ours to place
    uint64_t want = l.others;
    if (NameIs(slot.name, "Main XThread")) {
      want = uint64_t(1) << l.main;
      ++mains;
    } else if (NameIs(slot.name, "JQ worker 1")) {
      want = uint64_t(1) << l.d3d;
      ++d3ds;
    } else if (REXCVAR_GET(wos_audio_cpu) && NameIs(slot.name, "Audio Worker")) {
      // fix102: the game's audio mixer gets the render core's twin (the render thread
      // waits most of each frame) instead of sharing the "others" CPUs with five
      // busy-waiting game threads: late mixes play as silence (crackle, slow music)
      want = uint64_t(1) << (l.render + 1);
      ++audios;
    }
    if (g_layout_applied[i] == want) continue;
    if (SetCpuMask(slot.pthread, want)) {
      g_layout_applied[i] = want;
      ++moved;
    }
  }
  if (first || moved) {
    static int logged = 0;
    if (logged++ < 8)
      REXLOG_INFO("[WoS-CPU] layout: render thread CPU {}, Main XThread CPU {} ({} found), "
                  "JQ worker 1 CPU {} ({} found), Audio Worker CPU {} ({} found), others mask "
                  "{:#x} of allowed {:#x}; {} thread(s) placed now",
                  l.render, l.main, mains, l.d3d, d3ds,
                  REXCVAR_GET(wos_audio_cpu) ? l.render + 1 : -1, audios, l.others, l.allowed,
                  moved);
  }
#endif
}

// fix114: file this frame's samples and wait times under slow or fast frames
void ProfFrameEnd(double ms, double ms_per_tick) {
  static const bool slow_profile = REXCVAR_GET(wos_slow_frame_profile);
  if (!slow_profile) return;
  const int cat = ms >= 18.5 ? 1 : 0;
  // the two threads' cumulative kind ticks now
  uint64_t kinds[kProfThreads][kKindCount] = {};
  bool found[kProfThreads] = {};
  const int count = std::min(g_slot_count.load(), kMaxThreads);
  for (int i = 0; i < count; ++i) {
    const int t = ProfIndex(g_slots[i].name);
    if (t < 0 || found[t]) continue;
    found[t] = true;
    for (int k = 0; k < kKindCount; ++k)
      kinds[t][k] = g_slots[i].ticks[k].load(std::memory_order_relaxed);
  }
  std::lock_guard lock(g_sample_lock);
  ++g_prof_frames[cat];
  g_prof_ms[cat] += ms;
  for (int t = 0; t < kProfThreads; ++t) {
    if (!found[t]) continue;
    for (int k = 0; k < kKindCount; ++k) {
      if (g_prof_kind_last[t][k])
        g_prof_kind_ms[cat][t][k] += double(kinds[t][k] - g_prof_kind_last[t][k]) * ms_per_tick;
      g_prof_kind_last[t][k] = kinds[t][k];
    }
    const uint64_t now5[5] = {g_sl0_calls[t].load(std::memory_order_relaxed),
                              g_sl0_ticks[t].load(std::memory_order_relaxed),
                              g_slt_calls[t].load(std::memory_order_relaxed),
                              g_slt_ticks[t].load(std::memory_order_relaxed),
                              g_slt_req_us[t].load(std::memory_order_relaxed)};
    const double scale[5] = {1.0, ms_per_tick, 1.0, ms_per_tick, 0.001};
    for (int k = 0; k < 5; ++k) {
      g_prof_sleep[cat][t][k] += double(now5[k] - g_sl_last[t][k]) * scale[k];
      g_sl_last[t][k] = now5[k];
    }
  }
  for (const ProfSample& ps : g_frame_samples) {
    ++g_prof[cat][ps.thread][ps.key];
    ++g_prof_samples[cat][ps.thread];
  }
  g_frame_samples.clear();
}

void NoteSwap() {
  ApplyCpuLayout();  // fix46
  // 0028: the hitch log is a diagnostic; skip it (lock, counter reads) when off.
  static const bool diagnostics =
      REXCVAR_GET(wos_diagnostics) || REXCVAR_GET(wos_profile);
  if (!diagnostics) {
    return;
  }
  static const double ms_per_tick = 1000.0 / double(sceKernelGetTscFrequency());
  const uint64_t now = __rdtsc();
  std::lock_guard lock(g_hitch_lock);
  const HitchSnapshot snapshot = TakeHitchSnapshot();
  if (g_last_swap) {
    const double ms = double(now - g_last_swap) * ms_per_tick;
    g_worst_ms = std::max(g_worst_ms, ms);
    if (ms >= 45.0) {
      ++g_hitches_45;
      g_hitches_60 += ms >= 60.0;
      g_hitches_90 += ms >= 90.0;
      if (g_hitches_logged < 24) {
        ++g_hitches_logged;
        const HitchSnapshot& p = g_last_snapshot;
        char text[256];
        std::snprintf(text, sizeof text,
                      "hitch: frame %.0f ms; pipelines +%llu (%.1f ms), shader translation %.1f ms, "
                      "textures +%llu (%.1f ms), file reads +%llu (%.1f ms)",
                      ms, (unsigned long long)(snapshot.gpu[0] - p.gpu[0]),
                      (snapshot.gpu[1] - p.gpu[1]) / 1e6, (snapshot.gpu[2] - p.gpu[2]) / 1e6,
                      (unsigned long long)(snapshot.gpu[3] - p.gpu[3]),
                      (snapshot.gpu[4] - p.gpu[4]) / 1e6,
                      (unsigned long long)(snapshot.file_calls - p.file_calls),
                      double(snapshot.file_ticks - p.file_ticks) * ms_per_tick);
        REXLOG_INFO("[WoS-HITCH] {}", text);
        prospero_startup_log(text);
      }
    }
  }
  if (g_last_swap)
    ProfFrameEnd(double(now - g_last_swap) * ms_per_tick, ms_per_tick);  // fix114
  g_last_swap = now;
  g_last_snapshot = snapshot;
}

// The busy threads of the last window, one line each, in the log and in klog
void Report() {
  WriteProfile();
  {
    std::lock_guard lock(g_hitch_lock);
    char text[160];
    std::snprintf(text, sizeof text, "frames of 45+ ms: %u (60+: %u, 90+: %u), longest %.0f ms",
                  g_hitches_45, g_hitches_60, g_hitches_90, g_worst_ms);
    REXLOG_INFO("[WoS-HITCH] {}", text);
    prospero_startup_log(text);
    g_hitches_45 = g_hitches_60 = g_hitches_90 = g_hitches_logged = 0;
    g_worst_ms = 0;
  }
  {
    // The sampled threads' most frequent calls of the window
    std::lock_guard lock(g_sample_lock);
    const int count = std::min(g_slot_count.load(), kMaxThreads);
    for (int i = 0; i < count; ++i) {
      if (!g_sample_total[i]) {
        continue;
      }
      std::vector<std::pair<uint32_t, uint32_t>> top(g_samples[i].begin(), g_samples[i].end());
      std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
      const size_t shown = std::strncmp(g_slots[i].name, "Main", 4) == 0 ? 16 : 5;
      char text[512];
      int at = std::snprintf(text, sizeof text, "samples '%s' %08X (%u):", g_slots[i].name, g_slots[i].handle, g_sample_total[i]);
      for (size_t t = 0; t < top.size() && t < shown && at < int(sizeof text) - 24; ++t) {
        at += std::snprintf(text + at, sizeof text - at, " %08X %.0f%%", top[t].first,
                            100.0 * top[t].second / g_sample_total[i]);
      }
      REXLOG_INFO("[WoS-THREADS] {}", text);
      prospero_startup_log(text);
      g_samples[i].clear();
      g_sample_total[i] = 0;
    }
  }
  {
    // fix114: slow vs fast frames
    std::lock_guard lock(g_sample_lock);
    const uint32_t fs = g_prof_frames[1], ff = g_prof_frames[0];
    if (fs + ff) {
      REXLOG_INFO("[WoS-SLOW] fix114: {} slow frames (>= 18.5 ms, avg {:.1f} ms), {} fast (avg {:.1f} ms)",
                  fs, fs ? g_prof_ms[1] / fs : 0.0, ff, ff ? g_prof_ms[0] / ff : 0.0);
      for (int t = 0; t < kProfThreads; ++t) {
        std::string line;
        for (int cat = 1; cat >= 0; --cat) {
          const double n = cat ? fs : ff;
          if (!n) continue;
          double blocked = 0;
          for (int k = 0; k < kKindCount; ++k) blocked += g_prof_kind_ms[cat][t][k];
          const double frame_ms = (cat ? g_prof_ms[1] : g_prof_ms[0]) / n;
          line += std::format(" | {} frames: running {:.1f} ms", cat ? "slow" : "fast",
                              frame_ms - blocked / n);
          for (int k = 0; k < kKindCount; ++k)
            if (g_prof_kind_ms[cat][t][k] / n >= 0.05)
              line += std::format(", {} {:.2f}", kKindNames[k], g_prof_kind_ms[cat][t][k] / n);
          const double* sl = g_prof_sleep[cat][t];
          line += std::format(" (Sleep(0) {:.0f}x {:.2f} ms, timed sleeps {:.1f}x {:.2f} ms for {:.2f} asked)",
                              sl[0] / n, sl[1] / n, sl[2] / n, sl[3] / n, sl[4] / n);
        }
        REXLOG_INFO("[WoS-SLOW] {} thread per frame{}", kProfNames[t], line);
        // where the samples go in slow frames, against fast frames
        std::vector<std::pair<ProfKey, uint32_t>> top(g_prof[1][t].begin(), g_prof[1][t].end());
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
        const double ns = double(std::max<uint64_t>(1, g_prof_samples[1][t]));
        const double nf = double(std::max<uint64_t>(1, g_prof_samples[0][t]));
        for (size_t i = 0; i < top.size() && i < 14; ++i) {
          const auto it = g_prof[0][t].find(top[i].first);
          const double fast_share = it == g_prof[0][t].end() ? 0.0 : 100.0 * it->second / nf;
          REXLOG_INFO("[WoS-SLOW]   {} {:08X} <- {:08X} <- {:08X}: slow {:.1f}%  fast {:.1f}%",
                      kProfNames[t], top[i].first.a, top[i].first.b, top[i].first.c,
                      100.0 * top[i].second / ns, fast_share);
        }
      }
    }
    for (int cat = 0; cat < 2; ++cat) {
      g_prof_frames[cat] = 0;
      g_prof_ms[cat] = 0;
      for (int t = 0; t < kProfThreads; ++t) {
        g_prof[cat][t].clear();
        g_prof_samples[cat][t] = 0;
        for (int k = 0; k < kKindCount; ++k) g_prof_kind_ms[cat][t][k] = 0;
        for (int k = 0; k < 5; ++k) g_prof_sleep[cat][t][k] = 0;
      }
    }
  }
  static const double frequency = double(sceKernelGetTscFrequency());
  const uint64_t now = __rdtsc();
  const int count = std::min(g_slot_count.load(), kMaxThreads);
  struct Line {
    double running;
    std::string text;
  };
  std::vector<Line> lines;
  for (int i = 0; i < count; ++i) {
    Slot& slot = g_slots[i];
    const double window = double(now - slot.reported_at);
    if (window <= 0) {
      continue;
    }
    double shares[kKindCount];
    uint64_t calls[kKindCount];
    double blocked = 0;
    for (int k = 0; k < kKindCount; ++k) {
      const uint64_t ticks = slot.ticks[k].load(std::memory_order_relaxed);
      const uint64_t total_calls = slot.calls[k].load(std::memory_order_relaxed);
      shares[k] = 100.0 * double(ticks - slot.reported_ticks[k]) / window;
      calls[k] = total_calls - slot.reported_calls[k];
      slot.reported_ticks[k] = ticks;
      slot.reported_calls[k] = total_calls;
      blocked += shares[k];
    }
    slot.reported_at = now;
    const double running = std::max(0.0, 100.0 - blocked);
    if (running < 5.0) {
      continue;
    }
    char text[256];
    int at = std::snprintf(text, sizeof text, "thread '%s' (%08X): running %.0f%%", slot.name,
                           slot.handle, running);
    for (int k = 0; k < kKindCount && at < int(sizeof text); ++k) {
      if (shares[k] >= 1.0 || calls[k] >= 1000) {
        at += std::snprintf(text + at, sizeof text - at, ", %s %.0f%% (%llu)", kKindNames[k],
                            shares[k], (unsigned long long)calls[k]);
      }
    }
    lines.push_back({running, text});
  }
  std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.running > b.running; });
  for (const Line& line : lines) {
    REXLOG_INFO("[WoS-THREADS] {}", line.text);
    prospero_startup_log(line.text.c_str());
  }
  (void)frequency;
}

}  // namespace wos::threads

// The wrapped kernel calls
#define WOS_WRAP_KERNEL(name, kind)                                                     \
  extern "C" void __real___imp__##name(PPCContext& __restrict ctx, uint8_t* base);     \
  extern "C" void __wrap___imp__##name(PPCContext& __restrict ctx, uint8_t* base) {    \
    const uint64_t start = __rdtsc();                                                   \
    __real___imp__##name(ctx, base);                                                    \
    wos::threads::Account(wos::threads::kind, __rdtsc() - start, &ctx);                 \
  }

WOS_WRAP_KERNEL(KeWaitForSingleObject, kWait)
WOS_WRAP_KERNEL(KeWaitForMultipleObjects, kWait)
WOS_WRAP_KERNEL(NtWaitForSingleObjectEx, kWait)
WOS_WRAP_KERNEL(NtWaitForMultipleObjectsEx, kWait)
// KeDelayExecutionThread: Sleep(0) made cheap (wos_sleep0_spin_us).
//
// The game's job system waits with Sleep(0) polls: the main thread ("Game Master") on
// its jobs, and every idle job worker on new work (829635F0 / 82962FD0, measured with
// wos_sleep_probe: ~2,000-2,500 a second on the main thread, ~4,000 on each worker). On
// the 360 a Sleep(0) is a yield of a microsecond or two. The runtime answers it with
// sched_yield, or a 100 us sleep at below-normal priority, and on the PS5 each one came
// to some 160 us: the main thread spent ~40% of the time inside them (headless gameplay
// ran 50-60 fps with no thread busy). With wos_sleep0_spin_us > 0, a non-alertable
// zero-length delay instead spins that many microseconds with PAUSE and returns, as the
// 360's yield would. 0 (the default) keeps the runtime's behaviour.
REXCVAR_DEFINE_INT32(wos_sleep0_spin_us, 0, "WoS",
                     "Sleep(0) from the game: spin this many microseconds instead of "
                     "yielding to the PS5 scheduler (0: the runtime's yield)");
extern "C" void __real___imp__KeDelayExecutionThread(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __wrap___imp__KeDelayExecutionThread(PPCContext& __restrict ctx, uint8_t* base) {
  const uint64_t start = __rdtsc();
  static const int32_t spin_us_all = REXCVAR_GET(wos_sleep0_spin_us);
  // fix114: which of the profiled threads this is (-1: neither), and whether it spins its
  // Sleep(0)s: the draw / main thread have CPUs of their own (fix46), so a spin there takes
  // nothing from the audio worker (the menu crackle came from spinning on every thread)
  static thread_local int t_prof = -2;
  static thread_local bool t_spin = false, t_yield = false;
  if (t_prof == -2) {
    const wos::threads::Slot* slot = wos::threads::CurrentSlot();
    t_prof = slot ? wos::threads::ProfIndex(slot->name) : -1;
    t_spin = (t_prof == 1 && REXCVAR_GET(wos_spin_sleep0_draw)) ||
             (t_prof == 0 && REXCVAR_GET(wos_spin_sleep0_main));
    t_yield = t_prof == 0 && REXCVAR_GET(wos_main_sleep0_yield);
    if (t_prof >= 0) {
      // fix115: the runtime turns a Sleep(0) of a thread at or below kBelowNormal into a 100 us
      // sleep, of any other thread into a yield - which one each of these two threads gets
      const auto* xt = rex::system::XThread::GetCurrentThread();
      REXLOG_INFO("[WoS-SLEEP] fix115: {} thread ({}) priority {} (below-normal is {}): Sleep(0) = {}{}",
                  wos::threads::kProfNames[t_prof], slot->name, xt ? xt->priority() : -99,
                  int(rex::thread::ThreadPriority::kBelowNormal),
                  xt && xt->priority() <= int(rex::thread::ThreadPriority::kBelowNormal) ? "100 us sleep" : "yield",
                  t_yield ? " -> yield (wos_main_sleep0_yield)" : t_spin ? " -> spin" : "");
    }
  }
  const int32_t spin_us = spin_us_all > 0 ? spin_us_all : (t_spin ? 2 : 0);
  uint64_t interval_be = ~0ull;
  if (ctx.r5.u32 && (t_prof >= 0 || spin_us > 0))
    std::memcpy(&interval_be, rex::memory::GuestPtr(base, ctx.r5.u32), sizeof(interval_be));
  const bool zero = interval_be == 0;
  bool done = false;
  if (spin_us > 0 && ctx.r4.u32 == 0 && ctx.r5.u32) {
    uint64_t interval = interval_be;
    if (interval == 0) {  // zero either way round: no byte swap needed
      const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(spin_us);
      do {
        _mm_pause();
        _mm_pause();
        _mm_pause();
        _mm_pause();
      } while (std::chrono::steady_clock::now() < until);
      ctx.r3.u64 = 0;  // X_STATUS_SUCCESS
      done = true;
    }
  }
  // fix115: wos_main_sleep0_yield - the main thread (a CPU of its own) yields as a
  // normal-priority thread does, instead of the below-normal 100 us sleep
  if (!done && t_yield && zero && ctx.r4.u32 == 0) {
    sched_yield();
    __sync_synchronize();
    ctx.r3.u64 = 0;  // X_STATUS_SUCCESS
    done = true;
  }
  if (!done) __real___imp__KeDelayExecutionThread(ctx, base);
  const uint64_t took = __rdtsc() - start;
  if (t_prof >= 0) {  // fix114
    using namespace wos::threads;
    if (zero) {
      g_sl0_calls[t_prof].store(g_sl0_calls[t_prof].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      g_sl0_ticks[t_prof].store(g_sl0_ticks[t_prof].load(std::memory_order_relaxed) + took, std::memory_order_relaxed);
    } else if (interval_be != ~0ull) {
      const int64_t v = int64_t(__builtin_bswap64(interval_be));  // 100 ns units, < 0 relative
      const uint64_t req_us = v < 0 ? uint64_t(-v) / 10 : 0;
      g_slt_calls[t_prof].store(g_slt_calls[t_prof].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      g_slt_ticks[t_prof].store(g_slt_ticks[t_prof].load(std::memory_order_relaxed) + took, std::memory_order_relaxed);
      g_slt_req_us[t_prof].store(g_slt_req_us[t_prof].load(std::memory_order_relaxed) + req_us, std::memory_order_relaxed);
    }
  }
  wos::threads::Account(wos::threads::kSleep, took, &ctx);
}
WOS_WRAP_KERNEL(NtYieldExecution, kSleep)

// NtPulseEvent made cheap (wos_fast_pulse).
//
// Headless gameplay with Sleep(0) spinning still ran 45-55 fps with the main thread
// 97% busy - and 40-53% of its samples sat right after its call to NtPulseEvent
// (82B16C9C, in the game's PulseEvent wrapper sub_82B16C88): the job queue pulses its
// workers' events several times per job it posts. The runtime's pulse (patch 0007)
// sets the event, then yields until 10 us have passed - but each sched_yield on the
// PS5 takes ~160 us, so every pulse cost about that. With wos_fast_pulse:
//   - an auto-reset (synchronization) event is just set: it wakes one waiter, or the
//     next one to wait (a job worker that wakes to an empty queue simply waits again);
//   - a manual-reset (notification) event is set, the waiters are given 10 us by a
//     PAUSE spin instead of yields, and it is reset, as a pulse leaves it.
REXCVAR_DEFINE_BOOL(wos_fast_pulse, false, "WoS",
                    "NtPulseEvent without the runtime's yields (each ~160 us on the PS5)");
extern "C" void __real___imp__NtPulseEvent(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __wrap___imp__NtPulseEvent(PPCContext& __restrict ctx, uint8_t* base) {
  const uint64_t start = __rdtsc();
  static const bool fast = REXCVAR_GET(wos_fast_pulse);
  if (!fast) {
    __real___imp__NtPulseEvent(ctx, base);
    wos::threads::Account(wos::threads::kSleep, __rdtsc() - start, &ctx);
    return;
  }
  const uint32_t handle = ctx.r3.u32, previous_state_va = ctx.r4.u32;
  auto event = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(handle);
  if (!event) {
    ctx.r3.u64 = 0xC0000008u;  // X_STATUS_INVALID_HANDLE
    return;
  }
  uint32_t type = 0;
  event->Query(&type, nullptr);
  event->Set(0, false);
  if (type == 0) {  // manual reset: give the waiters their moment, then reset
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(10);
    do {
      _mm_pause();
      _mm_pause();
      _mm_pause();
      _mm_pause();
    } while (std::chrono::steady_clock::now() < until);
    event->Reset();
  }
  if (previous_state_va) {
    const uint32_t one = __builtin_bswap32(1u);  // as the runtime's pulse reports
    std::memcpy(rex::memory::GuestPtr(base, previous_state_va), &one, sizeof(one));
  }
  ctx.r3.u64 = 0;  // X_STATUS_SUCCESS
  wos::threads::Account(wos::threads::kSleep, __rdtsc() - start, &ctx);
}
WOS_WRAP_KERNEL(RtlEnterCriticalSection, kCriticalSection)
WOS_WRAP_KERNEL(KfAcquireSpinLock, kSpinLock)
WOS_WRAP_KERNEL(KeAcquireSpinLockAtRaisedIrql, kSpinLock)
WOS_WRAP_KERNEL(NtReadFile, kFile)
WOS_WRAP_KERNEL(NtCreateFile, kFile)
