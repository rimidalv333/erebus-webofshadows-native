// gpu/draw.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <rex/types.h>

#include "gpu/render_thread.h"

namespace eot::gpu {

struct VideoState;

void ReplayDrawLocked(VideoState &s, const DrawPacket &pk);
// WoS port (fix111): the render thread's half of a deferred capture (queue order, before replay)
bool FinishDeferredDraw(VideoState &s, DrawPacket &pk);
void ReplayClearLocked(VideoState &s, const ClearPacket &pk);
void ReplayResolveLocked(VideoState &s, const ResolvePacket &pk);

struct FloatConstantDirty {
  u64 vs = ~0ull;
  u64 ps = ~0ull;
};

void DrawGuestPrimitives(u32 device_va, u32 primitive_type, u32 start_vertex,
                         u32 vertex_count, FloatConstantDirty constants);

// fix118: one draw of several {first vertex, vertex count} ranges of the same list
// primitive (the game's window-strip lists, wos_strip_merge)
void DrawGuestPrimitiveRanges(u32 device_va, u32 primitive_type, const u32 *ranges, u32 range_count,
                              FloatConstantDirty constants);

void QueueGuestUpDraw(u32 device_va, u32 primitive_type, u32 vertex_count, u32 stride,
                      u32 data_va, FloatConstantDirty constants);
void FlushPendingUpDraw();

void DrawGuestIndexedPrimitives(u32 device_va, u32 primitive_type, i32 base_vertex,
                                u32 start_index, u32 index_count,
                                FloatConstantDirty constants);
void PrefetchIndexProbes(u32 device_va, u32 start_index, u32 index_count);
u64 DrawSortKey(const DrawPacket &pk, u32 *depth_func = nullptr);

void FlushPendingTransitions(VideoState &s);
void FlushGeometryStaging(VideoState &s);
struct GeometryCacheSizes {
  u64 indexUpload = 0, indexVram = 0, vertexUpload = 0, vertexVram = 0;
  u32 indexChunks = 0, vertexChunks = 0;
};
void GeometryCacheBytes(GeometryCacheSizes *out);
void ClearGuestTargets(u32 device_va, u32 flags, u32 rect_va, u32 color_va, float z, u32 stencil);

void ResolveGuest(u32 device_va, u32 flags, u32 src_rect_va, u32 dest_texture_va,
                  u32 dest_point_va, u32 dest_level, u32 clear_color_va, float clear_z);

}
