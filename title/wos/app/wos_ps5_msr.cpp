// WebOfShadowsRecomp for the PS5 - per-CPU locks for the game's "interrupts off"
// regions (see wos/shim/wos_msr.h).
//
// SPDX-License-Identifier: MIT

#include <atomic>
#include <cstdint>
#include <thread>

#include <x86intrin.h>

#include <rex/system/xthread.h>

namespace {

struct alignas(64) CpuLock {
  std::atomic<uintptr_t> owner{0};
};

// The guest's six CPUs, and one for a thread with no guest CPU
CpuLock g_locks[7];

thread_local uint32_t t_depth = 0;
thread_local CpuLock* t_lock = nullptr;
thread_local char t_token;  // its address is this thread's identity

CpuLock& LockForThisThread() {
  uint32_t cpu = 6;
  if (auto* thread = rex::system::XThread::GetCurrentThread()) {
    cpu = thread->active_cpu();
    if (cpu > 5) {
      cpu = 6;
    }
  }
  return g_locks[cpu];
}

}  // namespace

extern "C" uint64_t wos_msr_check(void) {
  // 0x8000 (EE set: interrupts on) unless this thread is in a region
  return t_depth ? 0 : 0x8000;
}

extern "C" void wos_msr_enter(void) {
  if (t_depth++) {
    return;
  }
  CpuLock& lock = LockForThisThread();
  const uintptr_t me = reinterpret_cast<uintptr_t>(&t_token);
  uint32_t spins = 0;
  for (;;) {
    uintptr_t expected = 0;
    if (lock.owner.load(std::memory_order_relaxed) == 0 &&
        lock.owner.compare_exchange_weak(expected, me, std::memory_order_acquire)) {
      break;
    }
    if (++spins < 64) {
      _mm_pause();
    } else {
      std::this_thread::yield();
    }
  }
  t_lock = &lock;
}

extern "C" void wos_msr_leave(void) {
  if (!t_depth) {
    return;  // a restore without an earlier disable: nothing held
  }
  if (--t_depth) {
    return;
  }
  if (t_lock) {
    t_lock->owner.store(0, std::memory_order_release);
    t_lock = nullptr;
  }
}
