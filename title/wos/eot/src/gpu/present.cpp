// gpu/present.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <plume_render_interface.h>
#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#else
#include <plume_vulkan.h>
#endif

#include "core/logging.h"
#include "gpu/memory_report.h"
#include "gpu/backend.h"
#include "gpu/render_thread.h"
#include "gpu/pipeline/pso_precache.h"
#include "core/profiling.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/gpu_timing.h"
#include "gpu/imgui_overlay.h"
#include "gpu/patches/aspect_ratio.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/settings.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/surfaces.h"
#include "gpu/taa.h"
#include "gpu/textures.h"
#include "gpu/trace.h"

#if !defined(__PROSPERO__)
#include <SDL3/SDL.h>
#endif
#include <cmath>
#include <rex/cvar.h>

#if defined(__PROSPERO__)
// WoS port: frame pacing against the display (wos/app/wos_ps5_pacing.cpp, wos_even_pacing)
extern "C" void wos_pace_before_present();
extern "C" void wos_pace_presented();
#endif

namespace eot::gpu {

namespace {

bool ReportSwapChainFailure(VideoState &s, const char *operation) {
  bool device_removed = false;
#if defined(EOT_D3D12)
  auto *device = static_cast<plume::D3D12Device *>(s.device.get());
  const HRESULT reason = device && device->d3d ? device->d3d->GetDeviceRemovedReason() : S_OK;
  if (FAILED(reason)) {
    device_removed = true;
    EOT_ERROR("[present] {} failed; D3D12 device removed reason {:#010x}", operation,
              static_cast<u32>(reason));
  } else {
    EOT_WARN("[present] {} failed with the D3D12 device still alive", operation);
  }
#else
  EOT_WARN("[present] {} failed (the swap chain may be out of date)", operation);
#endif
  DrainHostDebugMessages(s, operation);
  return device_removed;
}

void DisableFailedDevice(VideoState &s) {
  s.ready = false;
  for (bool &submitted : s.command_list_submitted)
    submitted = false;
}

bool HandleResize(VideoState &s) {
  const bool requested = s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (!requested && !s.swap_chain->needsResize())
    return !s.swap_framebuffers.empty();

#if defined(EOT_D3D12)
  auto *d3d_device = static_cast<plume::D3D12Device *>(s.device.get());
  if (d3d_device && d3d_device->d3d &&
      FAILED(d3d_device->d3d->GetDeviceRemovedReason())) {
    ReportSwapChainFailure(s, "swap-chain resize requested after device removal");
    DisableFailedDevice(s);
    return false;
  }
#endif

  SubmitOpenListLocked(s);
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.command_list_submitted[i]) {
      s.queue->waitForCommandFence(s.fences[i].get());
      GpuTimingCollect(s, i);
      s.command_list_submitted[i] = false;
    }
  }

#if !defined(EOT_D3D12)
  auto *vk_queue = static_cast<plume::VulkanCommandQueue *>(s.queue.get());
  VkResult idle_result = VK_ERROR_DEVICE_LOST;
  if (vk_queue && vk_queue->queue) {
    const std::scoped_lock queue_lock(*vk_queue->queue->mutex);
    idle_result = vkQueueWaitIdle(vk_queue->queue->vk);
  }
  if (idle_result != VK_SUCCESS) {
    EOT_ERROR("[present] vkQueueWaitIdle before resize failed with error {:#010x}; "
              "renderer disabled",
              static_cast<u32>(idle_result));
    ReportSwapChainFailure(s, "present queue idle wait");
    DisableFailedDevice(s);
    return false;
  }
#endif

  s.swap_framebuffers.clear();
  s.render_semaphores.clear();
  if (!s.swap_chain->resize()) {
    const bool nonzero_size = s.swap_chain->getWidth() && s.swap_chain->getHeight();
    if (nonzero_size)
      ReportSwapChainFailure(s, "swap-chain resize");
#if defined(EOT_D3D12)
    if (nonzero_size) {
      auto *device = static_cast<plume::D3D12Device *>(s.device.get());
      if (!device || !device->d3d || FAILED(device->d3d->GetDeviceRemovedReason())) {
        s.ready = false;
        return false;
      }
      auto *failed = static_cast<plume::D3D12SwapChain *>(s.swap_chain.get());
      const plume::RenderSwapChainDesc replacement_desc = failed->desc;
      const bool vsync_enabled = failed->isVsyncEnabled();
      s.swap_chain.reset();
      auto replacement = s.queue->createSwapChain(replacement_desc);
      auto *candidate = replacement
                            ? static_cast<plume::D3D12SwapChain *>(replacement.get())
                            : nullptr;
      bool textures_ready = candidate && candidate->textures.size() >= candidate->desc.textureCount;
      if (textures_ready) {
        for (u32 i = 0; i < candidate->desc.textureCount; ++i)
          textures_ready = textures_ready && candidate->textures[i].d3d != nullptr;
      }
      if (!replacement || replacement->isEmpty() || !textures_ready) {
        if (replacement) {
          auto *bad = candidate;
          if (bad->textures.size() < bad->desc.textureCount)
            bad->desc.textureCount = static_cast<u32>(bad->textures.size());
        }
        EOT_ERROR("[present] failed to recreate the D3D12 swap chain; renderer disabled");
        s.ready = false;
        return false;
      }
      s.swap_chain = std::move(replacement);
      s.swap_chain->setVsyncEnabled(vsync_enabled);
      if (!BuildSwapFramebuffers(s)) {
        ReportSwapChainFailure(s, "replacement swap-chain framebuffer rebuild");
        s.resize_requested.store(true, std::memory_order_release);
        return false;
      }
      EOT_WARN("[present] recovered from ResizeBuffers failure with a fresh swap chain");
      return true;
    }
#else
    if (nonzero_size)
      s.resize_requested.store(true, std::memory_order_release);
#endif
    return false;
  }
  if (s.swap_chain->isEmpty()) {
    s.resize_requested.store(true, std::memory_order_release);
    return false;
  }
  if (!BuildSwapFramebuffers(s)) {
    ReportSwapChainFailure(s, "swap-chain framebuffer rebuild");
    s.resize_requested.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

u32 EnsureGammaLutLocked(VideoState &s) {
  if (s.gamma_mode == VideoState::GammaMode::None || !Settings::PresentGamma())
    return kInvalidDescriptorIndex;
  HostTexture &lut = s.gamma_lut;
  if (!lut.valid()) {
    plume::RenderTextureDesc desc;
    desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
    desc.width = 1024;
    desc.height = 1;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.format = plume::RenderFormat::R16G16B16A16_UNORM;
    desc.committed = true;
    lut.texture = CreateHostTexture(s.device.get(), desc, "gamma-lut");
    if (!lut.valid())
      return kInvalidDescriptorIndex;
    lut.format = desc.format;
    lut.viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    lut.width = desc.width;
    lut.height = desc.height;
    lut.depth = 1;
    lut.mipLevels = 1;
    lut.arraySize = 1;
    lut.layout = plume::RenderTextureLayout::UNKNOWN;
    s.gamma_lut_dirty = true;
  }
  if (s.gamma_lut_dirty) {
    UploadAlloc staging;
    if (!UploadAllocate(1024 * 8, kTexturePlacementAlignment, &staging))
      return kInvalidDescriptorIndex;
    auto *dst = reinterpret_cast<uint16_t *>(staging.cpu);
    for (u32 v = 0; v < 1024; ++v) {
      for (u32 c = 0; c < 3; ++c) {
        u32 out;
        if (s.gamma_mode == VideoState::GammaMode::Pwl) {
          const u32 i = v >> 3, f = v & 7;
          out = u32(s.gamma_pwl[c][i][0]) + (u32(s.gamma_pwl[c][i][1]) * f) / 8;
        } else {
          const u32 i = v >> 2, f = v & 3;
          const i32 a = s.gamma_table[c][i];
          const i32 b = s.gamma_table[c][std::min(i + 1, 255u)];
          out = static_cast<u32>(std::max(0, a + ((b - a) * i32(f)) / 4));
        }
        dst[v * 4 + c] = static_cast<uint16_t>(std::min(out, 65535u));
      }
      dst[v * 4 + 3] = 0xFFFF;
    }
    EOT_DEBUG("[present] gamma LUT rebuilt ({}): r(0)={:.4f} r(32)={:.4f} r(128)={:.4f} "
             "r(512)={:.4f} r(1023)={:.4f}",
             s.gamma_mode == VideoState::GammaMode::Pwl ? "pwl" : "table",
             dst[0] / 65535.0f, dst[32 * 4] / 65535.0f, dst[128 * 4] / 65535.0f,
             dst[512 * 4] / 65535.0f, dst[1023 * 4] / 65535.0f);
    if (FILE *f = std::fopen("logs/gamma_lut.txt", "w")) {
      for (u32 v = 0; v < 1024; ++v)
        std::fprintf(f, "%u %u %u\n", dst[v * 4], dst[v * 4 + 1], dst[v * 4 + 2]);
      std::fclose(f);
    }
    TransitionLocked(s, lut, plume::RenderTextureLayout::COPY_DEST);
    s.command_list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(lut.texture.get(), 0, 0),
        plume::RenderTextureCopyLocation::PlacedFootprint(staging.buffer, lut.format, 1024, 1, 1,
                                                          1024, staging.offset),
        0, 0, 0);
    s.gamma_lut_dirty = false;
  }
  TransitionLocked(s, lut, plume::RenderTextureLayout::SHADER_READ);
  return BindTextureSRVLocked(s, lut);
}

f64 g_pace_ms = 0.0;

void CollectRenderWorkLocked(VideoState &s, bool record) {
  if (!record) {
    s.surface_work_window.clear();
    s.texture_work_window.clear();
    s.render_area_window.clear();
  }

  std::vector<u64> active_surface_keys;
  for (auto &[key, owned] : s.surfaces) {
    GuestSurface &surface = *owned;
    const u64 events = surface.perfDraws + surface.perfClears + surface.perfResolves;
    if (record && events) {
      active_surface_keys.push_back(key);
      auto &stats = s.surface_work_window[key];
      stats.isDepth = surface.isDepth;
      stats.format = surface.isDepth ? surface.depthFormat : surface.colorFormat;
      stats.baseTile = surface.baseTile;
      if (!stats.minWidth) {
        stats.minWidth = stats.maxWidth = surface.width;
        stats.minHeight = stats.maxHeight = surface.height;
      } else {
        stats.minWidth = std::min(stats.minWidth, surface.width);
        stats.maxWidth = std::max(stats.maxWidth, surface.width);
        stats.minHeight = std::min(stats.minHeight, surface.height);
        stats.maxHeight = std::max(stats.maxHeight, surface.height);
      }
      stats.allocWidth = surface.allocWidth;
      stats.allocHeight = surface.allocHeight;
      stats.hostWidth = surface.host.width;
      stats.hostHeight = surface.host.height;
      stats.samples = surface.host.sampleCount;
      stats.draws += surface.perfDraws;
      stats.clears += surface.perfClears;
      stats.resolves += surface.perfResolves;
    }
    surface.perfDraws = 0;
    surface.perfClears = 0;
    surface.perfResolves = 0;
  }
  if (!active_surface_keys.empty()) {
    std::sort(active_surface_keys.begin(), active_surface_keys.end());
    u64 signature = 1469598103934665603ull;
    for (u64 key : active_surface_keys) {
      signature ^= key;
      signature *= 1099511628211ull;
    }
    s.render_area_window[signature]++;
  }

  for (auto &[header_va, shared] : s.textures) {
    GuestTexture &texture = *shared;
    const u64 events = texture.perfSamples + texture.perfResolves + texture.perfDeadResolves;
    if (record && events) {
      u64 key = 1469598103934665603ull;
      const u64 identity[] = {texture.va, texture.width, texture.height,
                              static_cast<u32>(texture.format)};
      for (u64 value : identity) {
        key ^= value;
        key *= 1099511628211ull;
      }
      auto &stats = s.texture_work_window[key];
      stats.va = texture.va;
      stats.width = texture.width;
      stats.height = texture.height;
      stats.hostWidth = texture.host.width;
      stats.hostHeight = texture.host.height;
      stats.format = static_cast<u32>(texture.format);
      stats.samples += texture.perfSamples;
      stats.resolves += texture.perfResolves;
      stats.deadResolves += texture.perfDeadResolves;
    }
    texture.perfSamples = 0;
    texture.perfResolves = 0;
    texture.perfDeadResolves = 0;
  }
}

void LogRenderAreaLocked(VideoState &s, u64 frames) {
  struct SurfaceWork {
    u64 key;
    const VideoState::SurfaceWorkStats *stats;
    u64 rank;
  };
  std::vector<SurfaceWork> surfaces;
  for (auto &[key, stats] : s.surface_work_window) {
    const u64 pixels = u64(std::max(stats.hostWidth, 1u)) * std::max(stats.hostHeight, 1u) *
                       std::max(stats.samples, 1u);
    const u64 draw_equivalent = stats.draws + stats.clears * 4 + stats.resolves * 4;
    surfaces.push_back({key, &stats, pixels * draw_equivalent});
  }
  std::sort(surfaces.begin(), surfaces.end(), [](const SurfaceWork &a, const SurfaceWork &b) {
    return a.rank > b.rank;
  });

  if (!surfaces.empty()) {
    std::vector<std::pair<u64, u64>> areas(s.render_area_window.begin(),
                                           s.render_area_window.end());
    std::sort(areas.begin(), areas.end(), [](const auto &a, const auto &b) {
      return a.second > b.second;
    });
    std::string area_details;
    for (size_t i = 0; i < std::min<size_t>(areas.size(), 3); ++i) {
      if (i)
        area_details += ", ";
      area_details += std::format("{:016x} {:.0f}%", areas[i].first,
                                  100.0 * areas[i].second / frames);
    }
    std::string details;
    const f64 n = static_cast<f64>(frames);
    const size_t count = std::min<size_t>(surfaces.size(), 6);
    for (size_t i = 0; i < count; ++i) {
      const auto &stats = *surfaces[i].stats;
      if (i)
        details += " | ";
      const std::string dimensions =
          stats.minWidth == stats.maxWidth && stats.minHeight == stats.maxHeight
              ? std::format("{}x{}", stats.minWidth, stats.minHeight)
              : std::format("{}-{}x{}-{}", stats.minWidth, stats.maxWidth, stats.minHeight,
                            stats.maxHeight);
      details += std::format(
          "{}{}:t{} {} alloc {}x{} host {}x{}@{}x d{:.1f} r{:.2f} c{:.2f}",
          stats.isDepth ? 'z' : 'c', stats.format, stats.baseTile, dimensions,
          stats.allocWidth, stats.allocHeight, stats.hostWidth, stats.hostHeight, stats.samples,
          stats.draws / n, stats.resolves / n, stats.clears / n);
    }
    EOT_DEBUG("[render-area] {} target signatures (top: {}) | {} targets | {}", areas.size(),
              area_details, surfaces.size(), details);
  }

  std::vector<const VideoState::TextureWorkStats *> textures;
  for (auto &[key, stats] : s.texture_work_window) {
    if (stats.resolves)
      textures.push_back(&stats);
  }
  std::sort(textures.begin(), textures.end(), [](const auto *a, const auto *b) {
    const u64 a_pixels = u64(std::max(a->hostWidth, 1u)) * std::max(a->hostHeight, 1u);
    const u64 b_pixels = u64(std::max(b->hostWidth, 1u)) * std::max(b->hostHeight, 1u);
    return a_pixels * a->resolves > b_pixels * b->resolves;
  });
  if (!textures.empty()) {
    std::string details;
    const f64 n = static_cast<f64>(frames);
    const size_t count = std::min<size_t>(textures.size(), 6);
    for (size_t i = 0; i < count; ++i) {
      const auto &texture = *textures[i];
      if (i)
        details += " | ";
      const f64 dead_percent = texture.resolves
                                   ? 100.0 * texture.deadResolves / texture.resolves
                                   : 0.0;
      details += std::format("{:#x} {}x{} host {}x{} f{} r{:.2f}/f dead{:.0f}% sample{:.1f}/f",
                             texture.va, texture.width, texture.height, texture.hostWidth,
                             texture.hostHeight, texture.format, texture.resolves / n,
                             dead_percent, texture.samples / n);
    }
    EOT_DEBUG("[resolve-work] {} active destinations | {}", textures.size(), details);
  }

  s.surface_work_window.clear();
  s.texture_work_window.clear();
  s.render_area_window.clear();
}

constexpr u32 kLiveWindow = 30;

void PublishLiveStatsLocked(const VideoState &s, const PerfCounters &p) {
  static u64 reset = ~0ull;
  static PerfCounters prev;
  if (reset != s.perf_resets) {
    reset = s.perf_resets;
    prev = PerfCounters{};
  }
  if (p.frames < prev.frames + kLiveWindow)
    return;
  const f64 frames = static_cast<f64>(p.frames - prev.frames);
  const f64 gpu_frames = static_cast<f64>(std::max(1u, p.gpu_frames - prev.gpu_frames));
  const auto per_frame = [&](f64 now, f64 before) { return static_cast<f32>((now - before) / frames); };
  LiveStats live;
  live.wall_ms = per_frame(p.frame_ms, prev.frame_ms);
  live.gpu_ms = static_cast<f32>((p.gpu_ms - prev.gpu_ms) / gpu_frames);
  live.capture_ms = per_frame(p.capture_ms, prev.capture_ms);
  live.present_wait_ms = per_frame(p.present_wait_ms, prev.present_wait_ms);
  live.draw_ms = per_frame(p.draw_ms, prev.draw_ms);
  live.record_ms = per_frame(p.record_ms + p.rec_state_ms + p.rec_bind_ms,
                             prev.record_ms + prev.rec_state_ms + prev.rec_bind_ms);
  live.draws = static_cast<u32>((p.draws - prev.draws) / frames);
  live.resolves = static_cast<u32>((p.resolves - prev.resolves) / frames);
  std::vector<std::tuple<f64, u32, u32>> cats;
  for (const auto &[cat, v] : p.gpu_cats) {
    f64 ms = v.first;
    u32 draws = v.second;
    if (const auto it = prev.gpu_cats.find(cat); it != prev.gpu_cats.end()) {
      ms -= it->second.first;
      draws -= it->second.second;
    }
    if (ms <= 0.0)
      continue;
    cats.emplace_back(ms / gpu_frames, static_cast<u32>(draws / frames), cat);
  }
  std::sort(cats.begin(), cats.end(),
            [](const auto &a, const auto &b) { return std::get<0>(a) > std::get<0>(b); });
  for (const auto &[ms, draws, cat] : cats) {
    if (live.row_count >= LiveStats::kMaxRows || (ms < 0.02 && live.row_count >= 4))
      break;
    LiveStats::Row &row = live.rows[live.row_count++];
    const std::string name = GpuCategoryName(cat);
    std::snprintf(row.name, sizeof(row.name), "%s", name.c_str());
    row.ms = static_cast<f32>(ms);
    row.draws = draws;
  }
  prev = p;
  PublishLiveStats(live);
}

void LogPerfLocked(VideoState &s) {
  MemoryReportTick(s);
  const i32 every = Settings::PerfFrames();
  PerfCounters &p = s.perf;
  const auto now = std::chrono::steady_clock::now();
  if (p.last_present.time_since_epoch().count() != 0)
    p.frame_ms += std::chrono::duration<f64, std::milli>(now - p.last_present).count();
  p.last_present = now;
  p.frames++;

  const i32 hitch_ms = Settings::HitchMs();
  {
    const f64 wall = p.frame_ms - s.perf_prev_frame.frame_ms;
    if (wall >= 0.0 && s.frame_walls.size() < 4096)
      s.frame_walls.push_back(static_cast<f32>(wall));
    const PerfCounters &q = s.perf_prev_frame;
    if (p.uploads >= q.uploads && p.upload_ms >= q.upload_ms && p.upload_ms - q.upload_ms > p.upload_frame_max_ms) {
      p.upload_frame_max_ms = p.upload_ms - q.upload_ms;
      p.upload_frame_max = p.uploads - q.uploads;
    }
  }
  if (hitch_ms > 0 && p.last_present.time_since_epoch().count() != 0) {
    const PerfCounters &q = s.perf_prev_frame;
    const f64 wall = p.frame_ms - q.frame_ms;
    const f64 paced = g_pace_ms - q.pace_ms;
    if (wall - paced > static_cast<f64>(hitch_ms)) {
      EOT_DEBUG("[hitch] frame {} threads: capture {:.2f} present-wait {:.2f} worker-idle {:.2f} "
                "blit {:.2f} house {:.2f} | gpu {:.2f} ({} frames collected) | transfers {} "
                "evicted {} vtx-miss {} idx-miss {} pso-binds {} dead-resolves {} | "
                "guest+other {:.2f}",
                s.guest_frames, p.capture_ms - q.capture_ms, p.present_wait_ms - q.present_wait_ms,
                p.worker_idle_ms - q.worker_idle_ms, p.present_blit_ms - q.present_blit_ms,
                p.present_house_ms - q.present_house_ms, p.gpu_ms - q.gpu_ms,
                p.gpu_frames - q.gpu_frames, p.surface_transfers - q.surface_transfers,
                p.textures_evicted - q.textures_evicted,
                p.vertex_cache_misses - q.vertex_cache_misses,
                p.index_cache_misses - q.index_cache_misses,
                p.pipeline_bind_calls - q.pipeline_bind_calls, p.dead_resolves - q.dead_resolves,
                wall - (p.capture_ms - q.capture_ms) - (p.present_wait_ms - q.present_wait_ms));
#if defined(__PROSPERO__)
      // The console's logs are what players send: a slow frame, and how much of it
      // went to building pipelines on this thread, at info (the first 400)
      static u32 told = 0;
      if (told++ < 400) {
        const PsoPrecacheStats pool = PsoPrecacheGetStats();
        EOT_INFO("[hitch] frame {} took {:.0f} ms: pso {:.0f} ms ({} built here), draw {:.0f}, upload {:.0f} ({} tex), "
                 "resolve {:.0f}, guest+other {:.0f} | pipelines pending: recorded {} predicted {} background {}",
                 s.guest_frames, wall, p.pso_ms - q.pso_ms, p.psos - q.psos, p.draw_ms - q.draw_ms,
                 p.upload_ms - q.upload_ms, p.uploads - q.uploads, p.resolve_ms - q.resolve_ms,
                 wall - (p.capture_ms - q.capture_ms) - (p.present_wait_ms - q.present_wait_ms), pool.recordedPending,
                 pool.priorityPending, pool.backgroundPending);
      }
#endif
      EOT_DEBUG("[hitch] frame {} took {:.1f} ms: draw {:.2f} ({} draws) upload {:.2f} ({} tex) "
                "resolve {:.2f} ({}) link {:.2f} ({}) pso {:.2f} ({}) guest d3d {:.2f} ({} calls) "
                "| acquire {:.2f} submit {:.2f} fence {:.2f} pace {:.2f} | new host objects: "
                "{} tex ({} surface {} mirror {} guest, {} recycled) {} views {} fb, {} parked | "
                "vtx {} KB idx {} KB "
                "const {} KB | live {} tex {} surf, pool {} | resolve: mirror {:.2f} fb {:.2f} bind "
                "{:.2f} | scans: alias {:.2f} msaa {:.2f} | "
                "unaccounted {:.2f}",
                s.guest_frames, wall, p.draw_ms - q.draw_ms, p.draws - q.draws,
                p.upload_ms - q.upload_ms, p.uploads - q.uploads, p.resolve_ms - q.resolve_ms,
                p.resolves - q.resolves, p.link_ms - q.link_ms, p.links - q.links,
                p.pso_ms - q.pso_ms, p.psos - q.psos, p.guest_d3d_ms - q.guest_d3d_ms,
                p.guest_d3d_calls - q.guest_d3d_calls, p.acquire_ms - q.acquire_ms,
                p.submit_ms - q.submit_ms, p.fence_ms - q.fence_ms, g_pace_ms - q.pace_ms,
                p.host_textures - q.host_textures, p.host_tex_surface - q.host_tex_surface,
                p.host_tex_mirror - q.host_tex_mirror, p.host_tex_guest - q.host_tex_guest,
                p.host_tex_recycled - q.host_tex_recycled,
                p.host_views - q.host_views,
                p.host_framebuffers - q.host_framebuffers, p.host_parked - q.host_parked,
                (p.vertex_bytes - q.vertex_bytes) / 1024, (p.index_bytes - q.index_bytes) / 1024,
                (p.constant_bytes - q.constant_bytes) / 1024, p.live_textures,
                p.live_surfaces, p.pool_size, p.resolve_mirror_ms - q.resolve_mirror_ms,
                p.resolve_fb_ms - q.resolve_fb_ms, p.resolve_bind_ms - q.resolve_bind_ms,
                p.alias_scan_ms - q.alias_scan_ms, p.msaa_scan_ms - q.msaa_scan_ms,
                wall - (p.draw_ms - q.draw_ms) - (p.upload_ms - q.upload_ms) -
                   (p.resolve_ms - q.resolve_ms) - (p.guest_d3d_ms - q.guest_d3d_ms) -
                   (p.acquire_ms - q.acquire_ms) - (p.submit_ms - q.submit_ms) -
                   (p.fence_ms - q.fence_ms) - (g_pace_ms - q.pace_ms));
    }
  }
  if (Settings::DiagScene()) {
    static u32 armed = 0;
    static u64 next_arm = 0;
    const u32 draws_this_frame =
        p.draws >= s.perf_prev_frame.draws ? p.draws - s.perf_prev_frame.draws : p.draws;
    if (armed < 3 && draws_this_frame >= 400 && s.guest_frames >= next_arm &&
        s.guest_frames >= static_cast<u64>(std::max(0, Settings::DiagSceneFrom()))) {
      armed++;
      next_arm = s.guest_frames + 400;
      Settings::ArmDiagFrame(static_cast<i32>(s.guest_frames + 2));
      EOT_INFO("[diag] scene reached at guest frame {}: logging the draws of frame {} ({} of 3)",
               s.guest_frames, s.guest_frames + 2, armed);
    }
  }
  {
    static bool was_recording = false;
    static u64 next_record = 0;
    static u32 recorded = 0;
    const bool recording = Settings::Record();
    if (recording != was_recording) {
      was_recording = recording;
      if (recording) {
        next_record = s.guest_frames;
        recorded = 0;
        EOT_INFO("[record] on at guest frame {}", s.guest_frames);
      } else {
        EOT_INFO("[record] off at guest frame {} ({} diagnostic frames)", s.guest_frames, recorded);
      }
    }
    if (recording && s.guest_frames >= next_record &&
        Settings::DiagFrame() < static_cast<i32>(s.guest_frames)) {
      next_record = s.guest_frames + 120;
      recorded++;
      Settings::ArmDiagFrame(static_cast<i32>(s.guest_frames + 2));
    }
  }
  CollectRenderWorkLocked(s, every > 0);
  {
    const PerfCounters &prev = s.perf_prev_frame;
    EOT_PLOT("host textures created", p.host_textures - prev.host_textures);
    EOT_PLOT("host views created", p.host_views - prev.host_views);
    EOT_PLOT("host framebuffers created", p.host_framebuffers - prev.host_framebuffers);
    EOT_PLOT("live guest textures", s.textures.size());
    EOT_PLOT("live surfaces", s.surfaces.size());
    EOT_PLOT("draws", p.draws - prev.draws);
    EOT_PLOT("resolves", p.resolves - prev.resolves);
  }
  EvictStaleGuestTextures(s);
  EvictHostTexturePool(s);
  p.live_textures = static_cast<u32>(s.textures.size());
  if (every > 0 && static_cast<i32>(p.frames) >= every) {
    u32 buckets[6] = {};
    u64 texels = 0, resolve_owned = 0, uploaded = 0;
    for (const auto &kv : s.textures) {
      const GuestTexture &t = *kv.second;
      if (t.resolveOwned) {
        resolve_owned++;
        continue;
      }
      if (!t.uploaded)
        continue;
      uploaded++;
      const u32 dim = std::max(t.width, t.height);
      buckets[dim >= 2048 ? 0 : dim >= 1024 ? 1 : dim >= 512 ? 2 : dim >= 256 ? 3 : dim >= 128 ? 4 : 5]++;
      texels += u64(t.width) * t.height;
    }
    EOT_DEBUG("[texstats] {} guest textures uploaded ({} resolve-owned): >=2048 {} | 1024 {} | 512 {} | "
              "256 {} | 128 {} | smaller {} | {:.1f} Mtexels base level",
              uploaded, resolve_owned, buckets[0], buckets[1], buckets[2], buckets[3], buckets[4],
              buckets[5], texels / 1e6);
  }
  p.live_surfaces = static_cast<u32>(s.surfaces.size());
  s.perf_prev_frame = p;
  s.perf_prev_frame.pace_ms = g_pace_ms;
  PublishLiveStatsLocked(s, p);

  f64 wall_p50 = 0, wall_p95 = 0, wall_p99 = 0, wall_max = 0;
  if (every > 0 && static_cast<i32>(p.frames) >= every && !s.frame_walls.empty()) {
    std::sort(s.frame_walls.begin(), s.frame_walls.end());
    const size_t n = s.frame_walls.size();
    auto at = [&](f64 q) { return s.frame_walls[std::min(n - 1, static_cast<size_t>(q * n))]; };
    wall_p50 = at(0.50);
    wall_p95 = at(0.95);
    wall_p99 = at(0.99);
    wall_max = s.frame_walls.back();
    s.frame_walls.clear();
  }
  if (every <= 0 || static_cast<i32>(p.frames) < every)
    return;
#if !defined(__PROSPERO__) // the PS5's one display is never hidden or unfocused
  static SDL_WindowFlags last_window = ~SDL_WindowFlags{0};
  int window_count = 0;
  if (SDL_Window **windows = SDL_GetWindows(&window_count)) {
    constexpr SDL_WindowFlags kWatched = SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_OCCLUDED | SDL_WINDOW_MINIMIZED;
    const SDL_WindowFlags now = window_count > 0 ? SDL_GetWindowFlags(windows[0]) & kWatched : 0;
    SDL_free(windows);
    if (now != last_window) {
      EOT_INFO("[window] {}{}{}", (now & SDL_WINDOW_INPUT_FOCUS) ? "focused" : "not focused",
               (now & SDL_WINDOW_OCCLUDED) ? ", occluded" : "", (now & SDL_WINDOW_MINIMIZED) ? ", minimized" : "");
      last_window = now;
    }
  }
#endif
  const f64 n = static_cast<f64>(p.frames);
#if defined(__PROSPERO__)
  {
    const PsoPrecacheStats pool = PsoPrecacheGetStats();
    EOT_INFO("[perf] {} frames, {:.1f} ms/frame (p95 {:.1f} max {:.1f}) | pso {:.0f} ms ({} built on the render thread) "
             "| pipelines built {} pending: recorded {} predicted {} background {}",
             p.frames, p.frame_ms / n, wall_p95, wall_max, p.pso_ms, p.psos, pool.built, pool.recordedPending,
             pool.priorityPending, pool.backgroundPending);
  }
#endif
  // WoS port (fix49): the full breakdown at info level - the console log is what we get
  EOT_INFO("[perf] {} frames, {:.2f} ms/frame wall (p50 {:.2f} p95 {:.2f} p99 {:.2f} max {:.2f}) | cpu ms/frame: capture {:.2f} wait {:.2f} idle {:.2f} draw {:.2f} ({} draws, {} noop; "
            "setup {:.2f} tgt {:.2f} psolk {:.2f} ({} memo, {} hot) streams {:.2f} [vtxcopy {:.2f}] const {:.2f} [float {:.2f} bind {:.2f}, {} file hits, {} mask-fast, {} mask-miss] rec {:.2f} [state {:.2f} vbind {:.2f}]; idx {:.2f} outside) resolve {:.2f} ({}; {} copies, {} handed, {} noop, {} dead, {} refresh, {} twin, {:.1f} alias deferred, {:.2f} alias copied; mirror {:.2f} fb {:.2f} bind {:.2f} alias {:.2f} msaa {:.2f}) upload {:.2f} ({}) link "
            "{:.2f} ({}) pso {:.2f} ({}) | guest d3d {:.2f} ({} calls) winmiss {} (@{:#x}) | idxcache hit {} miss {} evict {} vtxcache hit {} miss {} vram {}/{} "
            "| hostbind/f vb {:.1f}/{:.1f} (1s {:.1f}) ib {:.1f}/{:.1f} fb reuse {:.1f} tgtmemo {:.1f} (miss g{:.0f} w{:.0f} s{:.0f}) sorted {:.0f}/{:.0f} tex hit {:.1f}/{:.1f} pso/vp/sc/st {:.1f} (hit {:.1f})/{:.1f}/{:.1f}/{:.1f} barrier {:.1f}/{:.1f} "
            "| present acquire {:.2f} blit {:.2f} submit {:.2f} fence {:.2f} house {:.2f} pace {:.2f} | KB/frame vtx {} "
            "idx {} const {} | gpu {}",
            p.frames, p.frame_ms / n, wall_p50, wall_p95, wall_p99, wall_max, p.capture_ms / n, p.present_wait_ms / n, p.worker_idle_ms / n,
            p.draw_ms / n, p.draws / p.frames, p.draws_skipped / p.frames,
            p.setup_ms / n, p.replay_targets_ms / n,
            p.pso_lookup_ms / n, p.replay_memo_hits / p.frames, p.pipeline_hot_hits / p.frames, p.stream_ms / n,
            p.vertex_copy_ms / n, p.const_ms / n + p.const_float_ms / n,
            p.const_float_ms / n, p.bind_ms / n, p.const_file_hits / p.frames, p.const_file_clean_hits / p.frames,
            p.const_file_mask_misses,
            p.record_ms / n + p.rec_state_ms / n + p.rec_bind_ms / n, p.rec_state_ms / n, p.rec_bind_ms / n, p.index_ms / n, p.resolve_ms / n, p.resolves / p.frames, p.resolve_copies / p.frames, p.resolve_transfers / p.frames, p.resolve_noops / p.frames, p.dead_resolves / p.frames, p.resolve_refreshes / p.frames,
            p.surface_transfers / p.frames, static_cast<f64>(p.alias_deferred) / p.frames, static_cast<f64>(p.alias_copies) / p.frames, p.resolve_mirror_ms / n, p.resolve_fb_ms / n,
            p.resolve_bind_ms / n, p.alias_scan_ms / n, p.msaa_scan_ms / n, p.upload_ms / n,
            p.uploads, p.link_ms / n, p.links, p.pso_ms / n, p.psos, p.guest_d3d_ms / n,
            p.guest_d3d_calls / p.frames, g_device_block_misses.exchange(0, std::memory_order_relaxed),
            g_device_block_miss_offset.exchange(0, std::memory_order_relaxed),
            p.index_cache_hits / p.frames, p.index_cache_misses,
            p.index_cache_evictions, p.vertex_cache_hits / p.frames, p.vertex_cache_misses,
            p.geometry_vram_binds / p.frames, p.geometry_staging_binds / p.frames,
            static_cast<f64>(p.vertex_bind_calls) / n,
            static_cast<f64>(p.vertex_bind_requests) / n,
            static_cast<f64>(p.single_stream_draws) / n,
            static_cast<f64>(p.index_bind_calls) / n,
            static_cast<f64>(p.index_bind_requests) / n,
            static_cast<f64>(p.framebuffer_cache_hits) / n,
            static_cast<f64>(p.target_memo_hits) / n,
            static_cast<f64>(p.target_memo_miss_gen) / n,
            static_cast<f64>(p.target_memo_miss_words) / n,
            static_cast<f64>(p.target_memo_miss_sig) / n,
            static_cast<f64>(p.sorted_draws) / n, static_cast<f64>(p.sort_runs) / n,
            static_cast<f64>(p.texture_bind_hits) / n,
            static_cast<f64>(p.texture_bind_requests) / n,
            static_cast<f64>(p.pipeline_bind_calls) / n,
            static_cast<f64>(p.pipeline_bind_on_hit) / n,
            static_cast<f64>(p.viewport_bind_calls) / n,
            static_cast<f64>(p.scissor_bind_calls) / n,
            static_cast<f64>(p.stencil_ref_calls) / n,
            static_cast<f64>(p.texture_barrier_calls) / n,
            static_cast<f64>(p.texture_barrier_resources) / n,
            p.acquire_ms / n, p.present_blit_ms / n, p.submit_ms / n, p.fence_ms / n,
            p.present_house_ms / n, g_pace_ms / n,
            p.vertex_bytes / p.frames / 1024,
            p.index_bytes / p.frames / 1024, p.constant_bytes / p.frames / 1024,
            GpuTimingSummary(p));
  if (p.uploads || p.uploads_skipped)
    EOT_DEBUG("[uploads] {} ({} new, {} made again ({} reloaded by the game), {} of changed memory) in {:.1f} ms: {:.2f} M blocks at "
              "{:.1f} ns, CPU mip chains {:.1f} ms | worst frame {} uploads in {:.1f} ms | new textures the "
              "game's loader had in: under 50 ms before {}, 50-250 ms {}, 250 ms-1 s {}, over 1 s {}, "
              "not announced {} ({}) | preloaded {} | render-target headers cleared instead {} | mirrors let "
              "go: {} ({} as the game freed them)",
              p.uploads, p.uploads_new, p.uploads_again, p.uploads_again_reloaded, p.uploads_refresh, p.upload_ms,
              p.upload_blocks * 1e-6,
              p.upload_blocks ? (p.upload_ms - p.upload_synth_ms) * 1e6 / static_cast<f64>(p.upload_blocks) : 0.0,
              p.upload_synth_ms, p.upload_frame_max, p.upload_frame_max_ms, p.upload_lead[0], p.upload_lead[1],
              p.upload_lead[2], p.upload_lead[3], p.upload_lead[4], TakeUnannouncedShapes(), p.uploads_preloaded,
              p.uploads_skipped, p.textures_evicted,
              p.textures_released);
  LogRenderAreaLocked(s, p.frames);
  p = PerfCounters{};
  s.perf_resets++;
  p.last_present = now;
  g_pace_ms = 0.0;
}

void SleepUntil(std::chrono::steady_clock::time_point when) {
  using clock = std::chrono::steady_clock;
  constexpr auto kSpinTail = std::chrono::microseconds(400);
  const auto now = clock::now();
  if (when <= now)
    return;
#if defined(_WIN32)
  static HANDLE timer = []() -> HANDLE {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr,
                                      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return h ? h : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }();
  if (timer && when - now > kSpinTail) {
    LARGE_INTEGER due;
    due.QuadPart = -(std::chrono::duration_cast<std::chrono::nanoseconds>(when - now - kSpinTail)
                         .count() /
                     100);
    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
      WaitForSingleObject(timer, INFINITE);
  }
#elif defined(__APPLE__)
  const auto tail = ThreadSleepsPrecisely() ? kSpinTail : std::chrono::microseconds(4000);
  if (when - now > tail)
    std::this_thread::sleep_until(when - tail);
#else
  if (when - now > kSpinTail)
    std::this_thread::sleep_until(when - kSpinTail);
#endif
  while (clock::now() < when)
    std::this_thread::yield();
}

std::atomic<i64> g_movie_presented_ns{0};
constexpr auto kMovieHold = std::chrono::milliseconds(500);

bool MoviePresenting() {
  const i64 movie_ns = g_movie_presented_ns.load(std::memory_order_acquire);
  return movie_ns != 0 && std::chrono::steady_clock::now().time_since_epoch().count() - movie_ns <=
                              kMovieHold.count() * 1000000;
}

u32 PresentSyncInterval() { return Settings::Vsync() ? 1 : 0; }

void FrameLimitWait(const VideoState &s) {
  using clock = std::chrono::steady_clock;
  static clock::time_point deadline{};
  static i32 cadence_fps = 0;
  const i32 refresh = static_cast<i32>(s.display_refresh_hz);
  i32 fps = Settings::FpsLimit();
  if (MoviePresenting())
    fps = fps > 0 ? std::min(fps, refresh) : refresh;
  if (Settings::Vsync() && fps >= refresh)
    fps = 0;
  const auto now = clock::now();
  if (fps <= 0) {
    deadline = clock::time_point{};
    cadence_fps = 0;
    return;
  }
  const auto period =
      std::chrono::duration_cast<clock::duration>(std::chrono::duration<f64>(1.0 / fps));
  if (deadline == clock::time_point{} || cadence_fps != fps) {
    cadence_fps = fps;
    deadline = now + period;
  } else {
    deadline += period;
    if (now > deadline) {
      deadline = now;
      return;
    }
  }
  const auto before = clock::now();
  SleepUntil(deadline);
  g_pace_ms += std::chrono::duration<f64, std::milli>(clock::now() - before).count();
}

}

void PresentLocked(VideoState &s, u32 front_buffer_texture_va) {
  EOT_CPU_ZONE("PresentLocked");
  taa::EndFrame(s);
  velocity::EndFrame(s);
  s.guest_frames++;
  s.frame_clock[s.guest_frames % kFrameClockRing] =
      std::chrono::duration<f64>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire))
    return;
  DrainShaderGraveyardLocked(s);
  s.last_front_buffer_va = front_buffer_texture_va;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  GuestTexture *front = nullptr;
  if (front_buffer_texture_va) {
    front = GetGuestTexture(s, front_buffer_texture_va);
    if (front)
      FlushAliasCopy(s, *front);
    if (front && !front->host.valid())
      front = nullptr;
  }
  PreloadAnnouncedTextures(s, PsoCacheInLoadingScreen() ? 8.0 : 1.0);
  s.swap_chain->setVsyncEnabled(Settings::Vsync());
  if (!HandleResize(s)) {
    if (!s.ready)
      return;
    SubmitOpenListLocked(s);
    AdvanceAndWaitReused(s);
    return;
  }
  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  const u32 cur = s.recording_slot();
  u32 image = 0;
  bool acquired = false;
  {
    PerfScope perf_scope(s.perf.acquire_ms);
#if defined(__PROSPERO__)
    wos_pace_before_present();  // WoS port: not ahead of the display (wos_even_pacing)
#endif
    if (s.present_wait)
      s.swap_chain->wait();
    acquired = s.swap_chain->acquireTexture(s.acquire_semaphores[cur].get(), &image) &&
               image < s.swap_framebuffers.size();
  }
  if (!acquired) {
    u32 n;
    if (DiagShouldLog(0x8001, &n))
      EOT_WARN("[present] acquireTexture failed (minimised?)");
    s.resize_requested.store(true, std::memory_order_release);
    SubmitOpenListLocked(s);
    AdvanceAndWaitReused(s);
    return;
  }

  auto *cmd = s.command_list;
  const u64 blit_t0 = PerfNow();
  GpuTimingMark(s, cmd, kGpuCatPresent);
  GpuTimingDiagMark(s, cmd, "present");
  plume::RenderTexture *back = s.swap_chain->getTexture(image);
  plume::RenderTextureBarrier to_rt(back, plume::RenderTextureLayout::COLOR_WRITE);
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_rt, 1);
  u32 src_index = kInvalidDescriptorIndex;
  if (front) {
    TransitionLocked(s, front->host, plume::RenderTextureLayout::SHADER_READ);
    src_index = BindTextureSRVSwizzledLocked(s, front->host,
                                             SamplingSwizzle(*front, front->fetch[3] >> 1));
    front->lastUseFrame = s.guest_frames;
    GpuTimingMark(s, cmd, kGpuCatPresent);
  }
  const u32 lut_index = front ? EnsureGammaLutLocked(s) : kInvalidDescriptorIndex;

  const float out_w = static_cast<float>(s.swap_chain->getWidth());
  const float out_h = static_cast<float>(s.swap_chain->getHeight());
  float fit_w = out_w, fit_h = out_h, fit_x = 0.0f, fit_y = 0.0f;
  if (src_index != kInvalidDescriptorIndex) {
    ApplyAspectRatio();
    if (TakeMovieDrawnFlag())
      g_movie_presented_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                 std::memory_order_release);
    const float aspect = std::clamp(ConfiguredAspectRatio(), 0.5f, 4.5f);
    fit_h = out_w / aspect;
    if (fit_h > out_h) {
      fit_h = out_h;
      fit_w = out_h * aspect;
    }
    fit_x = (out_w - fit_w) * 0.5f;
    fit_y = (out_h - fit_h) * 0.5f;
  }

  cmd->setFramebuffer(s.swap_framebuffers[image].get());
  s.bound_framebuffer = nullptr;
  const bool uncovered =
      src_index == kInvalidDescriptorIndex || fit_w < out_w || fit_h < out_h;
  if (uncovered)
    cmd->clearColor(0, plume::RenderColor(0, 0, 0, 1), nullptr, 0);
  const i32 dump_every = Settings::DumpEvery();
  const u64 dump_phase = dump_every > 0 ? (s.presented_frames + 1) % static_cast<u64>(dump_every) : 1;
  if (front && ((dump_every > 0 && dump_phase < static_cast<u64>(std::max(1, Settings::DumpBurst()))) ||
                (Settings::DiagDump() && s.guest_frames == static_cast<u64>(Settings::DumpFrame())))) {
    ::mkdir("/app0/userdata/wos_recomp/dump", 0777);  // WoS port: beside the log
    const std::string path =
        std::format("/app0/userdata/wos_recomp/dump/frame_{}.ppm", s.presented_frames + 1);
    DumpHostTextureLocked(s, front->host, path.c_str(), 1.0f, lut_index,
                          SamplingSwizzle(*front, front->fetch[3] >> 1));
    cmd->setFramebuffer(s.swap_framebuffers[image].get());
  }
  if (src_index != kInvalidDescriptorIndex) {
    plume::RenderViewport vp(fit_x, fit_y, fit_w, fit_h, 0.0f, 1.0f);
    plume::RenderRect sc(static_cast<i32>(fit_x), static_cast<i32>(fit_y),
                         static_cast<i32>(fit_x + fit_w),
                         static_cast<i32>(fit_y + fit_h));
    cmd->setViewports(&vp, 1);
    cmd->setScissors(&sc, 1);
    plume::RenderPipeline *pso = GetBlitPipeline(s, plume::RenderFormat::B8G8R8A8_UNORM);
    cmd->setPipeline(pso);
    s.bound_pipeline = nullptr;
    CopyPushConstants pc;
    pc.resourceDescriptorIndex = src_index;
    pc.resourceDescriptorIndex2 = lut_index != kInvalidDescriptorIndex ? lut_index : 0u;
    pc.param0 = 1.0f;
    SelectPresentBlitMode(front->host.width, front->host.height, fit_w, fit_h, pc.extra);
    pc.colorAdjust[0] = static_cast<float>(std::clamp(Settings::Brightness(), -0.5, 0.5));
    pc.colorAdjust[1] = static_cast<float>(std::clamp(Settings::Contrast(), 0.25, 3.0));
    pc.colorAdjust[2] = static_cast<float>(std::clamp(Settings::Saturation(), 0.0, 3.0));
    pc.colorAdjust[3] = static_cast<float>(std::clamp(Settings::Gamma(), 0.4, 2.5));
    if (pc.colorAdjust[0] == 0.0f && pc.colorAdjust[1] == 1.0f && pc.colorAdjust[2] == 1.0f &&
        pc.colorAdjust[3] == 1.0f)
      pc.colorAdjust[3] = 0.0f;
    pc.param1 = lut_index != kInvalidDescriptorIndex ? 2.0f : 1.0f;
    pc.rect[0] = 0.0f;
    pc.rect[1] = 0.0f;
    pc.rect[2] = 1.0f;
    pc.rect[3] = 1.0f;
    cmd->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc, kCopyPushConstantByteOffset,
                                  sizeof(pc));
    cmd->drawInstanced(3, 1, 0, 0);
  } else if (front_buffer_texture_va) {
    u32 n;
    if (DiagShouldLog(0x8002, &n))
      EOT_WARN("[present] no front buffer mirror for {:#x}; presenting black",
               front_buffer_texture_va);
  }
  {
    const u32 out_w = s.swap_chain->getWidth(), out_h = s.swap_chain->getHeight();
    const plume::RenderViewport full(0.0f, 0.0f, static_cast<float>(out_w),
                                     static_cast<float>(out_h), 0.0f, 1.0f);
    const plume::RenderRect full_scissor(0, 0, static_cast<i32>(out_w), static_cast<i32>(out_h));
    cmd->setViewports(&full, 1);
    cmd->setScissors(&full_scissor, 1);
    RunOverlayDrawHook(cmd, s.swap_framebuffers[image].get(), out_w, out_h);
    s.bound_pipeline = nullptr;
    s.bound_framebuffer = nullptr;
  }

  plume::RenderTextureBarrier to_present(back, plume::RenderTextureLayout::PRESENT);
  cmd->barriers(plume::RenderBarrierStage::NONE, &to_present, 1);
  s.perf.present_blit_ms += static_cast<f64>(PerfNow() - blit_t0) * PerfMsPerTick();

  FlushGeometryStaging(s);
  GpuTimingFrameEnd(cmd);
  s.command_lists[cur]->end();
  s.command_list_open = false;
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *wait[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signal[] = {s.render_semaphores[image].get()};
  {
    PerfScope perf_scope(s.perf.submit_ms);
    s.queue->executeCommandLists(lists, 1, wait, 1, signal, 1, s.fences[cur].get());
    s.command_list_submitted[cur] = true;
#if defined(EOT_D3D12)
    const u32 sync_interval = PresentSyncInterval();
    const bool presented =
        sync_interval ? SUCCEEDED(static_cast<plume::D3D12SwapChain *>(s.swap_chain.get())->d3d->Present(sync_interval, 0))
                      : s.swap_chain->present(image, signal, 1);
#else
    const bool presented = s.swap_chain->present(image, signal, 1);
#if defined(__PROSPERO__)
    if (presented)
      wos_pace_presented();  // WoS port
#endif
#endif
    if (!presented) {
      const bool device_removed = ReportSwapChainFailure(s, "swap-chain present");
      if (device_removed) {
        DisableFailedDevice(s);
        return;
      }
      s.resize_requested.store(true, std::memory_order_release);
    }
  }
  s.presented_frames++;
  trace::PresentMarker(s.presented_frames);

  {
    PerfScope perf_scope(s.perf.fence_ms);
    AdvanceAndWaitReused(s);
  }
  {
    PerfScope house_scope(s.perf.present_house_ms);
    LogPerfLocked(s);
    PsoCacheFlushIfDirty(false);
    DrainHostDebugMessages(s, "present");
    RenderDocFrameBoundary(s.guest_frames);
  }
}

void Video::Present(u32 front_buffer_texture_va) {
  EOT_CPU_ZONE("Present");
  auto &s = state();
  static thread_local bool pinned = false;
  if (!pinned) {
    pinned = true;
    PinThreadToPhysicalCore(1, "the guest's rendering thread");
  }
  trace::EndFrame(s.guest_frames + 1);
  s.captured_presents++;
  velocity::BeginCaptureFrame(s);
  if (s.ready && !s.shutting_down.load(std::memory_order_acquire)) {
    if (RenderThreadActive()) {
      u64 seq = 0;
      {
        RenderEnqueue enqueue;
        enqueue.cmd().type = RenderCommandType::Present;
        enqueue.cmd().va = front_buffer_texture_va;
        seq = enqueue.commit();
      }
      PerfScope wait_scope(s.perf.present_wait_ms);
      RenderThreadWait(seq);
    } else {
      std::lock_guard lock(s.mutex);
      PresentLocked(s, front_buffer_texture_va);
    }
  }
  FrameLimitWait(s);
  EOT_FRAME_MARK();
}

void Video::PresentOverlayOnly() {
  auto &s = state();
  {
    std::lock_guard lock(s.mutex);
    PresentLocked(s, 0);
  }
  FrameLimitWait(s);
}

}

REXCVAR_DEFINE_STRING(wos_upscale, "lanczos", "WoS",
                      "Filter that scales the picture to the screen: bilinear, bicubic or lanczos")
    .allowed({"bilinear", "bicubic", "lanczos"});

namespace eot::gpu {

void SelectPresentBlitMode(u32 src_w, u32 src_h, float dst_w, float dst_h, float extra[4]) {
  extra[0] = extra[1] = extra[2] = extra[3] = 0.0f;
  if (!src_w || !src_h || dst_w <= 0.0f || dst_h <= 0.0f)
    return;
  const float rx = static_cast<float>(src_w) / dst_w;
  const float ry = static_cast<float>(src_h) / dst_h;
  if (rx >= 1.9f && ry >= 1.9f) {
    const float n = std::round(std::min(rx, ry));
    if (std::fabs(rx - n) < 0.06f && std::fabs(ry - n) < 0.06f) {
      extra[0] = 1.0f;
      extra[1] = n;
      return;
    }
  }
  if (rx < 0.98f && ry < 0.98f) {
    const std::string filter = REXCVAR_GET(wos_upscale);
    if (filter == "lanczos")
      extra[0] = 2.0f;
    else if (filter == "bicubic")
      extra[0] = 3.0f;
  }
}


// WoS port (fix34): the pad trigger (wos/app/wos_ps5_capture.cpp): eot_diag_dump's dumps -
// the scene after every 150 draws, each resolve, the textures post draws read, [stats],
// the presented frame - of the frame after next, named f<frame>_*.ppm.
void WosArmDiagnosticFrame() {
  auto &s = state();
  const u64 frame = s.guest_frames + 2;
  Settings::ArmDumpFrame(static_cast<i32>(frame));
  EOT_INFO("[diag] dumps armed for guest frame {}", frame);
}

}
