// WebOfShadowsRecomp for the PS5 - the game's "interrupts off" regions, per CPU.
//
// The Xbox 360 code brackets its atomic operations (lwarx/stwcx. loops) with
// mfmsr / mtmsrd r13,1 / mtmsrd rN,1: interrupts off on that hardware thread.
// ReXGlue's generated code turns each into the runtime's global critical region,
// one recursive mutex for every guest thread and the kernel: in Web of Shadows'
// city the main thread and its job workers spend much of their time queueing on it
// for each reference count and queue update (the samples behind patch 0021).
//
// Interrupts off only ever kept other code off the same hardware thread, so each
// of the guest's six CPUs gets a lock of its own here (wos/app/wos_ps5_msr.cpp):
// threads of one guest CPU exclude each other as on the console, threads of
// different CPUs do not. The atomics themselves stay host compare-and-swaps.
//
// Included after the generated precompiled header (wos/wos_game.cmake).
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

extern "C" uint64_t wos_msr_check(void);
extern "C" void wos_msr_enter(void);
extern "C" void wos_msr_leave(void);

#undef REX_CHECK_GLOBAL_LOCK
#undef REX_ENTER_GLOBAL_LOCK
#undef REX_LEAVE_GLOBAL_LOCK
#define REX_CHECK_GLOBAL_LOCK() wos_msr_check()
#define REX_ENTER_GLOBAL_LOCK() wos_msr_enter()
#define REX_LEAVE_GLOBAL_LOCK() wos_msr_leave()
