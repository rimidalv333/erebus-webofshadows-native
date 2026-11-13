// WebOfShadowsRecomp for the PS5 - the frame counter WebOfShadowsRecomp's
// unregistered guest function handler records into.
//
// WebOfShadowsRecomp's src/stubs/unregistered_func_handler.h is compiled from the
// builder's own checkout, unchanged. It includes "hooks/perf_monitor.h" for
// wos::perf::RecordFrame, which it calls from its hook on the game's swap
// (0x82AC4E48). Upstream that header is the Windows telemetry (window title,
// GetProcessTimes, the F4 frame pacer); wos/wos_game.cmake puts this folder
// first on the include path, so the handler gets this one: a frame count, which
// the title logs as frames a second (wos/app/wos_ps5_app.cpp).
//
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>

namespace wos::perf {

// Frames the game has handed the GPU to show: one per call of the kernel's
// VdSwap, which the title links wrapped (wos/app/wos_ps5_hooks.cpp)
inline std::atomic<uint64_t> g_total_frames{0};

inline void RecordSwap() {
  g_total_frames.fetch_add(1, std::memory_order_relaxed);
}

// Upstream's hook on 0x82AC4E48 (the game's own swap function) is installed in the
// guest function table, which only an indirect call goes through: the game calls
// that function directly, so it never counted. VdSwap counts instead.
inline void RecordFrame() {}

}  // namespace wos::perf
