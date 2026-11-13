// core/cpu.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#elif defined(_M_ARM64)
#include <intrin.h>
#endif

namespace eot::cpu {

inline void Relax() {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#elif defined(_M_ARM64)
  __yield();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield" ::: "memory");
#endif
}

inline void Prefetch(const void *p) {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_prefetch(static_cast<const char *>(p), _MM_HINT_T0);
#elif defined(_M_ARM64)
  __prefetch(p);
#else
  __builtin_prefetch(p, 0, 3);
#endif
}

inline uint64_t Timestamp() {
#if defined(__x86_64__) || defined(_M_X64)
  return __rdtsc();
#elif defined(_M_ARM64)
  return _ReadStatusReg(ARM64_CNTVCT);
#elif defined(__aarch64__)
  uint64_t v;
  __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
  return v;
#else
  return 0;
#endif
}

}
