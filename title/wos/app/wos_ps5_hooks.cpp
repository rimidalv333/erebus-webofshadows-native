// WebOfShadowsRecomp for the PS5 - the game's guest hooks that are not Windows code.
//
// Two of WebOfShadowsRecomp's hooks keep the game alive where its own code would
// fault, and the PS5 needs both. Upstream they lean on Windows: one patches
// rexruntime.dll's import table, the other catches faults with SEH (__try/
// __except), which a PS5 title has neither of. Here they are written again:
//
// 1. Invalid indirect call targets (upstream src/hooks/indirect_call_hooks.h).
//    The game sometimes calls through a null or wild function pointer. The
//    recompiled code resolves a target it has no table entry for through
//    rex::runtime::ResolveIndirectFunction; the title links with
//    --wrap=<its mangled name> (wos/wos_game.cmake), so those calls reach this
//    wrapper first, which answers a no-op for an address that cannot be code.
//
// 2. The hash table lookup at 0x82A1ACF0 (upstream src/hooks/hash_table_hooks.h).
//    The game walks a bucket chain that can hold a dangling node pointer.
//    Upstream reads it under SEH and gives up on a fault; here every node is
//    checked against the guest page table before it is read, which ends the walk
//    the same way without touching an unmapped page. The recompiled function is a
//    weak symbol (DEFINE_REX_FUNC), so this strong definition takes its place.
//
// And one for the log: the kernel's VdSwap, the game's "show this frame", is linked
// with --wrap=__imp__VdSwap, so each call counts a frame (the frames a second the
// title logs every ten seconds) before the runtime's VdSwap runs.
//
// SPDX-License-Identifier: MIT

#include <cstdint>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "hooks/perf_monitor.h"

namespace wos::hooks {

namespace {
rex::Runtime* g_runtime = nullptr;

// The guest address as the recompiled code reads it: the 0xE0000000 range sits
// 4 KiB further in the host's view on the PS5, as on Windows (REX_PHYS_HOST_OFFSET)
inline uint8_t* GuestPointer(uint8_t* base, uint32_t address) {
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
}

inline uint32_t LoadU32(uint8_t* base, uint32_t address) {
  return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(GuestPointer(base, address)));
}

inline void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  *reinterpret_cast<volatile uint32_t*>(GuestPointer(base, address)) = __builtin_bswap32(value);
}

// Whether [address, address + size) is committed and readable guest memory
bool Readable(uint32_t address, uint32_t size) {
  if (address < 0x00010000u || address >= 0xF0000000u || (address & 3u) ||
      uint64_t(address) + size > 0x100000000ull) {
    return false;
  }
  if (!g_runtime || !g_runtime->memory()) {
    return true;  // before the runtime is known: the upstream range check alone
  }
  auto* memory = g_runtime->memory();
  for (uint32_t probe : {address, address + size - 1}) {
    auto* heap = memory->LookupHeap(probe);
    uint32_t protect = 0;
    if (!heap || !heap->QueryProtect(probe, &protect) ||
        !(protect & rex::memory::kMemoryProtectRead)) {
      return false;
    }
  }
  return true;
}

void SafeNullFunction(PPCContext& ctx, uint8_t* base) {
  (void)base;
  REXLOG_WARN("[WoS-NULL] invalid indirect call skipped: target {:08X}, LR {:08X}, r3 {:08X}",
              ctx.last_indirect_target, static_cast<uint32_t>(ctx.lr), ctx.r3.u32);
  ctx.r3.u64 = 0;
}
}  // namespace

void SetRuntime(rex::Runtime* runtime) {
  g_runtime = runtime;
}

}  // namespace wos::hooks

// --- 1. Invalid indirect call targets ----------------------------------------

// rex::runtime::ResolveIndirectFunction(uint32_t), as the linker names it
extern "C" PPCFunc* __real__ZN3rex7runtime23ResolveIndirectFunctionEj(uint32_t guest_address);

extern "C" __attribute__((visibility("hidden"))) PPCFunc*
__wrap__ZN3rex7runtime23ResolveIndirectFunctionEj(uint32_t guest_address) {
  if (guest_address < 0x10000u || guest_address >= 0xFF000000u) {
    REXLOG_WARN("[WoS-RESOLVE] invalid indirect call target {:08X}: a no-op instead", guest_address);
    return &wos::hooks::SafeNullFunction;
  }
  PPCFunc* function = __real__ZN3rex7runtime23ResolveIndirectFunctionEj(guest_address);
  return function ? function : &wos::hooks::SafeNullFunction;
}

// --- 2. The hash table lookup at 0x82A1ACF0 ------------------------------------

// The kernel's critical sections, as the recompiled code calls them
extern "C" void __imp__RtlEnterCriticalSection(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__RtlLeaveCriticalSection(PPCContext& __restrict ctx, uint8_t* base);

// r3: the table, r4: the key. Returns the node (its reference count raised) or 0.
// The table's buckets start at +0x1C, 521 of them; a node is
// { +4 reference count, +8 next, +12 key }, as the game's own code has them.
extern "C" __attribute__((visibility("hidden"))) void sub_82A1ACF0(PPCContext& __restrict ctx,
                                                                   uint8_t* base) {
  using wos::hooks::LoadU32;
  using wos::hooks::Readable;
  using wos::hooks::StoreU32;
  const uint32_t table = ctx.r3.u32;
  const uint32_t key = ctx.r4.u32;
  const uint32_t bucket = table + (key % 521 + 7) * 4;
  if (!Readable(table, 4) || !Readable(bucket, 4)) {
    ctx.r3.u64 = 0;
    return;
  }

  ctx.r3.u64 = table;
  __imp__RtlEnterCriticalSection(ctx, base);

  uint32_t found = 0;
  uint32_t node = LoadU32(base, bucket);
  for (int guard = 0; guard < 1000 && node && Readable(node, 16); ++guard) {
    if (LoadU32(base, node + 12) == key) {
      StoreU32(base, node + 4, LoadU32(base, node + 4) + 1);
      found = node;
      break;
    }
    node = LoadU32(base, node + 8);
  }

  ctx.r3.u64 = table;
  __imp__RtlLeaveCriticalSection(ctx, base);
  ctx.r3.u64 = found;
}

// The kernel's VdSwap, counted (the recompiled code's calls reach this through
// --wrap=__imp__VdSwap; the runtime's own registration keeps the original)
extern "C" void __real___imp__VdSwap(PPCContext& __restrict ctx, uint8_t* base);
namespace wos::threads {
void Account(int kind, uint64_t ticks, const PPCContext* context);  // wos_ps5_threads.cpp (5: VdSwap)
void NoteSwap();  // wos_ps5_threads.cpp: long frames
}
namespace wos::pacing {
void NoteGameFrame();  // wos_ps5_pacing.cpp: what the display shows
}
namespace wos::capture {
void OnSwap();  // wos_ps5_capture.cpp
}
namespace wos::native_renderer {
bool Enabled();
}
extern "C" void __wrap___imp__VdSwap(PPCContext& __restrict ctx, uint8_t* base) {
  wos::perf::RecordSwap();
  wos::threads::NoteSwap();
  // fix52: L3+R3 captures on the emulated renderer too (its draws are recorded in
  // wos_ps5_native.cpp) - the same spot captured on both renderers shows which draws the
  // native path never gets (the black buildings' medium LOD)
  if (!wos::native_renderer::Enabled()) wos::capture::OnSwap();
  wos::pacing::NoteGameFrame();
  const uint64_t start = __builtin_ia32_rdtsc();
  __real___imp__VdSwap(ctx, base);
  wos::threads::Account(5, __builtin_ia32_rdtsc() - start, &ctx);
}

// The console's monotonic clock from the CPU's time-stamp counter.
//
// clock_gettime is a system call on the PS5: about 880 ns each (measured at
// start, wos_ps5_app.cpp's "costs" line), against some 20 ns on a PC's vDSO.
// The runtime reads the clock in every guest wait, timer and tick query, and
// the guest's threads make hundreds of thousands of those a second. The title
// links with --wrap=clock_gettime (wos/wos_game.cmake): the monotonic clocks are
// answered from the invariant TSC, anchored once to the kernel's reading, and
// every other clock goes to the kernel as before.
#include <ctime>
#include <x86intrin.h>

extern "C" uint64_t sceKernelGetTscFrequency(void);
extern "C" int __real_clock_gettime(clockid_t clock_id, struct timespec* tp);

namespace {
struct TscClock {
  uint64_t tsc_base = 0;
  uint64_t ns_base = 0;
  uint64_t frequency = 0;
  bool ready = false;
};

TscClock& GetTscClock() {
  static TscClock clock = [] {
    TscClock c;
    c.frequency = sceKernelGetTscFrequency();
    timespec now{};
    if (c.frequency >= 1000000 && __real_clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
      c.tsc_base = __rdtsc();
      c.ns_base = uint64_t(now.tv_sec) * 1000000000ull + uint64_t(now.tv_nsec);
      c.ready = true;
    }
    return c;
  }();
  return clock;
}
}  // namespace

extern "C" __attribute__((visibility("hidden"))) int __wrap_clock_gettime(clockid_t clock_id,
                                                                    struct timespec* tp) {
  // FreeBSD's monotonic clocks: CLOCK_MONOTONIC, _PRECISE and _FAST
  if (tp && (clock_id == CLOCK_MONOTONIC || clock_id == 11 || clock_id == 12)) {
    const TscClock& clock = GetTscClock();
    if (clock.ready) {
      const uint64_t ticks = __rdtsc() - clock.tsc_base;
      const uint64_t ns =
          clock.ns_base + uint64_t((unsigned __int128)ticks * 1000000000ull / clock.frequency);
      tp->tv_sec = time_t(ns / 1000000000ull);
      tp->tv_nsec = long(ns % 1000000000ull);
      return 0;
    }
  }
  return __real_clock_gettime(clock_id, tp);
}
