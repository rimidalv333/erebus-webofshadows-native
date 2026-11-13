// gpu/trace.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/trace.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "core/logging.h"
#include "gpu/settings.h"

namespace eot::gpu::trace {

namespace {

struct State {
  std::mutex mutex;
  std::vector<std::string> lines;
  std::atomic<u32> counters[static_cast<u32>(Counter::Count)] = {};
  std::atomic<i32> frames_left{-1};
  std::atomic<u64> frames_seen{0};
  std::atomic<bool> trace_on{false};
  std::atomic<bool> summary_on{false};
};

State &st() {
  static State s;
  return s;
}

void Refresh(State &s) {
  const i64 seen = static_cast<i64>(s.frames_seen.load(std::memory_order_relaxed));
  const bool trace_on =
      s.frames_left.load(std::memory_order_relaxed) > 0 && seen >= Settings::TraceStartFrame();
  s.trace_on.store(trace_on, std::memory_order_relaxed);
  g_trace_on.store(trace_on, std::memory_order_relaxed);
  const i32 summary_frames = Settings::SummaryFrames();
  const bool summary_on = summary_frames > 0 && seen < static_cast<i64>(summary_frames);
  s.summary_on.store(summary_on, std::memory_order_relaxed);
  g_summary_on.store(summary_on, std::memory_order_relaxed);
}

void InitOnce(State &s) {
  i32 expected = -1;
  if (s.frames_left.load(std::memory_order_relaxed) == expected) {
    s.frames_left.compare_exchange_strong(expected, Settings::TraceFrames(),
                                          std::memory_order_relaxed);
    Refresh(s);
    g_trace_ready.store(true, std::memory_order_release);
  }
}

const char *CounterName(Counter c) {
  switch (c) {
  case Counter::DrawVertices:
    return "draw";
  case Counter::DrawIndexed:
    return "drawIdx";
  case Counter::DrawDropped:
    return "dropped";
  case Counter::Clear:
    return "clear";
  case Counter::Resolve:
    return "resolve";
  case Counter::SetRenderTarget:
    return "setRT";
  case Counter::SetDepth:
    return "setDS";
  case Counter::SetTexture:
    return "setTex";
  case Counter::SetVertexShader:
    return "setVS";
  case Counter::SetPixelShader:
    return "setPS";
  case Counter::SetStreamSource:
    return "setVB";
  case Counter::SetIndices:
    return "setIB";
  case Counter::SetViewport:
    return "setVP";
  case Counter::BeginVertices:
    return "beginVerts";
  case Counter::ShaderMiss:
    return "shaderMiss";
  default:
    return "?";
  }
}

}

bool EnabledSlow() {
  auto &s = st();
  InitOnce(s);
  return s.trace_on.load(std::memory_order_relaxed);
}

void Line(std::string_view text) {
  auto &s = st();
  std::lock_guard lock(s.mutex);
  if (s.lines.size() < 20000)
    s.lines.emplace_back(text);
}

void BumpSlow(Counter c) {
  auto &s = st();
  InitOnce(s);
  if (!s.summary_on.load(std::memory_order_relaxed))
    return;
  s.counters[static_cast<u32>(c)].fetch_add(1, std::memory_order_relaxed);
}

void EndFrame(u64 frame_index) {
  auto &s = st();
  InitOnce(s);
  std::lock_guard lock(s.mutex);
  const u64 frames_seen = s.frames_seen.fetch_add(1, std::memory_order_relaxed) + 1;
  if (s.frames_left.load(std::memory_order_relaxed) > 0 &&
      static_cast<i64>(frames_seen) > Settings::TraceStartFrame()) {
    EOT_INFO("[d3d-trace] ---- frame {} begin ({} calls) ----", frame_index, s.lines.size());
    for (size_t i = 0; i < s.lines.size(); ++i)
      EOT_INFO("[d3d-trace] {:5} {}", i, s.lines[i]);
    EOT_INFO("[d3d-trace] ---- frame {} end ----", frame_index);
    s.frames_left.fetch_sub(1, std::memory_order_relaxed);
  }
  s.lines.clear();
  Refresh(s);
  const bool emit_summary = static_cast<i64>(frames_seen) <= Settings::SummaryFrames();
  std::string summary;
  for (u32 i = 0; i < static_cast<u32>(Counter::Count); ++i) {
    const u32 count = s.counters[i].exchange(0, std::memory_order_relaxed);
    if (emit_summary && count)
      summary += std::format(" {}={}", CounterName(static_cast<Counter>(i)), count);
  }
  if (emit_summary) {
    EOT_INFO("[d3d-frame] {}:{}", frame_index, summary);
  }
}

void PresentMarker(u64 frame_index) {
  if (!Settings::PresentFrameLog())
    return;
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  EOT_INFO("[present] frame {} t={}", frame_index,
           std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}
