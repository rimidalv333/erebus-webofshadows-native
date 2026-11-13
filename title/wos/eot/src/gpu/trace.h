// gpu/trace.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <atomic>
#include <format>
#include <string_view>

#include <rex/types.h>

namespace eot::gpu::trace {

enum class Counter : u32 {
  DrawVertices,
  DrawIndexed,
  DrawDropped,
  Clear,
  Resolve,
  SetRenderTarget,
  SetDepth,
  SetTexture,
  SetVertexShader,
  SetPixelShader,
  SetStreamSource,
  SetIndices,
  SetViewport,
  BeginVertices,
  ShaderMiss,
  Count
};

inline std::atomic<bool> g_trace_ready{false};
inline std::atomic<bool> g_trace_on{false};
inline std::atomic<bool> g_summary_on{false};
bool EnabledSlow();
void BumpSlow(Counter c);
inline bool Enabled() {
  if (!g_trace_ready.load(std::memory_order_relaxed))
    return EnabledSlow();
  return g_trace_on.load(std::memory_order_relaxed);
}
void Line(std::string_view text);
inline void Bump(Counter c) {
  if (!g_trace_ready.load(std::memory_order_relaxed) || g_summary_on.load(std::memory_order_relaxed))
    BumpSlow(c);
}
void EndFrame(u64 frame_index);
void PresentMarker(u64 frame_index);

}

#define EOT_TRACE_CALL(...)                                                                    \
  do {                                                                                         \
    if (::eot::gpu::trace::Enabled())                                                          \
      ::eot::gpu::trace::Line(std::format(__VA_ARGS__));                                       \
  } while (0)
