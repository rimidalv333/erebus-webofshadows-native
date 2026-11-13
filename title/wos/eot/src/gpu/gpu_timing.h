// gpu/gpu_timing.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <string>

#include <rex/types.h>
#include <vector>
#include "core/profiling.h"

namespace plume {
struct RenderCommandList;
}

namespace eot::gpu {

struct VideoState;
struct PerfCounters;

constexpr u32 kGpuCatOther = 0;
constexpr u32 kGpuCatShadow = 1;
constexpr u32 kGpuCatResolve = 2;
constexpr u32 kGpuCatPresent = 3;
constexpr u32 kGpuCatUpload = 4;
constexpr u32 kGpuCatResolveHw = 6;
constexpr u32 kGpuCatResolveDepth = 7;
constexpr u32 kGpuCatBroadcast = 8;
constexpr u32 kGpuCatTaa = 9;

u32 GpuTargetCategory(bool full_frame, bool has_depth, u32 color_count, u32 color0_host_format,
                      u32 samples, bool additive);
std::string GpuCategoryName(u32 cat);

void GpuTimingFrameBegin(VideoState &s, plume::RenderCommandList *cmd, u32 slot);
void GpuTimingMark(VideoState &s, plume::RenderCommandList *cmd, u32 cat);
void GpuTimingCountDraw(VideoState &s);
bool GpuTimingDiagActive(const VideoState &s);
void GpuTimingDiagMark(VideoState &s, plume::RenderCommandList *cmd, std::string tag);
void GpuTimingFrameEnd(plume::RenderCommandList *cmd);
void GpuTimingCollect(VideoState &s, u32 slot);
std::string GpuTimingSummary(const PerfCounters &p);

}

#if defined(EOT_PROFILING) && defined(REXGLUE_ENABLE_PROFILING) && defined(EOT_D3D12)
#define EOT_GPU_PROFILING 1
#else
#define EOT_GPU_PROFILING 0
#endif

#if EOT_GPU_PROFILING

#include <plume_d3d12.h>
#include <tracy/TracyD3D12.hpp>

namespace eot::gpu {

void InitGPUProfiler(ID3D12Device *device, ID3D12CommandQueue *queue);
TracyD3D12Ctx GpuProfilerCtx();
void SetGPUProfilerCommandList(ID3D12GraphicsCommandList *cmd);
ID3D12GraphicsCommandList *GpuProfilerCommandList();

}

#define EOT_GPU_ZONE(name)                                                                         \
  ZoneNamedN(___tracy_scoped_zone, name, TracyIsStarted);                                          \
  static constexpr tracy::SourceLocationData TracyConcat(__tracy_gpu_sloc, TracyLine){             \
      name, TracyFunction, TracyFile, (u32)TracyLine, 0};                                          \
  tracy::D3D12ZoneScope TracyConcat(__tracy_gpu_zone, TracyLine) {                                 \
    eot::gpu::GpuProfilerCtx(), eot::gpu::GpuProfilerCommandList(),                                \
        &TracyConcat(__tracy_gpu_sloc, TracyLine), TracyIsStarted                                  \
  }

#elif defined(EOT_PROFILING) && defined(REXGLUE_ENABLE_PROFILING)

#define EOT_GPU_ZONE(name) ZoneNamedN(___tracy_scoped_zone, name, TracyIsStarted)

#else

#define EOT_GPU_ZONE(name) ((void)0)

#endif

namespace eot::gpu {

struct LiveStats {
  u64 sequence = 0;
  f32 wall_ms = 0, gpu_ms = 0, capture_ms = 0, present_wait_ms = 0, draw_ms = 0, record_ms = 0;
  u32 draws = 0, resolves = 0;
  struct Row {
    char name[20] = {};
    f32 ms = 0;
    u32 draws = 0;
  };
  static constexpr u32 kMaxRows = 8;
  Row rows[kMaxRows];
  u32 row_count = 0;
};

void PublishLiveStats(const LiveStats &stats);
bool ReadLiveStats(LiveStats *out);

}

namespace eot::gpu {

std::string AdapterSensorsSummary();

}

namespace eot::gpu {

void NoteTextureResident(u32 header_va);

bool TakeTextureResidentAge(u32 header_va, f64 &age_ms);

bool UploadSeenBefore(u64 storage_key);

void NoteTextureReleased(u32 header_va);

void TakeReleasedTextures(std::vector<u32> &out);

bool TakeAnnouncedHeader(u32 &header_va);

void NoteUnannouncedUpload(u32 width, u32 height, u32 guest_format, bool tiled);
std::string TakeUnannouncedShapes();

}
