// gpu/pipeline/pipeline_cache.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <rex/types.h>

#include <plume_render_interface.h>

namespace eot::gpu {

struct VideoState;
struct InputLayout;

struct PipelineState {
  const plume::RenderShader *vs;
  const plume::RenderShader *ps;
  const InputLayout *layout;
  u64 vsHash;
  u64 psHash;
  u64 layoutKey;
  u32 spec;
  u32 strides[16];
  plume::RenderPrimitiveTopology topology;
  plume::RenderFormat rtFormats[4];
  u32 rtCount;
  plume::RenderFormat dsFormat;
  u32 sampleCount;
  plume::RenderCullMode cull;
  plume::RenderFrontFace frontFace;
  i32 depthBias;
  float slopeScaledDepthBias;
  float targetScale;
  bool depthClip;
  bool depthEnable;
  bool depthWrite;
  plume::RenderComparisonFunction depthFunc;
  bool stencilEnable;
  u8 stencilReadMask;
  u8 stencilWriteMask;
  u8 stencilRef;
  plume::RenderStencilFaceDesc stencilFront;
  plume::RenderStencilFaceDesc stencilBack;
  plume::RenderBlendDesc blend[4];
  bool alphaToCoverage;
  bool velocity;
};
constexpr size_t kPipelineKeyOffset = offsetof(PipelineState, vsHash);

enum class PsoSource : u8 { Draw = 0, CompiledIn = 1, LocalCsv = 2, Predicted = 3 };

void ZeroPipelineState(PipelineState &state);
u64 HashPipelineState(const PipelineState &state);

inline i32 PolygonOffsetUnits(float offset) {
  const float layers = std::ceil(std::fabs(offset) * static_cast<float>(1u << 21));
  const i32 units = static_cast<i32>(std::min(layers, 1.0e8f)) << 3;
  return offset < 0.0f ? -units : units;
}

void CanonicalizePipelineState(PipelineState &st, u32 spec_mask, u32 stream_mask);

plume::RenderPipeline *GetOrCreatePipeline(VideoState &s, const PipelineState &state,
                                           bool worker = false,
                                           PsoSource source = PsoSource::Draw,
                                           u16 template_index = 0xFFFF);

void PsoCachePrecache();

void PsoCacheFlushIfDirty(bool force);
void PipelineCacheCounts(u32 *alive, u32 *used);

void PsoCacheSetLoadingScreen(bool on);
bool PsoCacheInLoadingScreen();

void PsoCacheOnPackageLoad(u32 id, bool level);
bool PsoCacheLevelKnown();
bool PsoCacheHoldPackage(u32 id);
bool PsoCacheWaitsAllowed();

}
