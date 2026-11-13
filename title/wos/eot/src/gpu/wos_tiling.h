// gpu/wos_tiling.h - WoS port: Xbox 360 predicated tiling, drawn whole.
//
// Web of Shadows renders its scene with predicated tiling: a 1280x256 EDRAM render
// target and depth buffer, D3DDevice_BeginTiling with three 1280x256 tile rects, the
// scene's draws issued once, and D3DDevice_EndTiling resolving each tile (its rect in
// screen space, its dest point at the tile's origin). The GPU replays the draws once per
// tile; this renderer sees each draw once, so the surfaces bound when tiling begins are
// given the whole tiling extent (1280x720) instead of the EDRAM tile's size, and the
// per-tile resolves then read their part of one full-size image.
#pragma once

#include <rex/types.h>

namespace eot::gpu::wos_tiling {

// From the BeginTiling hook, before the original runs: the surfaces bound to the device
// take (width, height), the union of the tile rects, from now on.
void Begin(u32 device_va, u32 width, u32 height);
// True while BeginTiling's own clear runs: that clear covers one tile's size (the EDRAM
// surface), the whole extent here.
void SetClearWhole(bool on);
bool ClearWhole();
// Applied to every surface header read: the size word of a tiled surface grown to its
// tiling extent.
void ApplyExtent(u32 surface_va, u32 words[5]);

// The header words the last draw into a tiled surface used; a resolve of that surface
// reads the same ones (EndTiling resolves it after D3D has re-written its header, and a
// different word set is a different host surface - the scene, drawn, resolved as black).
void NoteDrawWords(u32 surface_va, const u32 words[5]);
void UseDrawWords(u32 surface_va, u32 words[5]);
// EndTiling (sub_82AC9170) in progress: its per-tile resolves become one whole resolve.
void SetEndTiling(bool on);
bool InEndTiling();

}  // namespace eot::gpu::wos_tiling
