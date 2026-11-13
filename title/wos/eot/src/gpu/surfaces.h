// gpu/surfaces.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <rex/types.h>

#include "gpu/resources.h"

namespace eot::gpu {

struct VideoState;

GuestSurface *GetGuestSurface(VideoState &s, u32 surface_va);
bool ReadSurfaceHeaderWords(u32 surface_va, u32 words[5]);
GuestSurface *GetGuestSurfaceWords(VideoState &s, u32 surface_va, const u32 words[5]);

plume::RenderFormat SurfaceHostFormat(const GuestSurface &surface);
bool IsTextureCameraSurface(const GuestSurface &surface);
bool IsShadowTile(const GuestSurface &surface);
const plume::RenderTextureView *DepthTargetView(VideoState &s, HostTexture &host);

GuestSurface *FindMultisampleAliasSource(VideoState &s, const GuestSurface &alias);

plume::RenderFramebuffer *GetFramebuffer(VideoState &s, HostTexture *const color[4],
                                         u32 color_count, HostTexture *depth);

HostTexture &SurfaceContentImage(GuestSurface &surf);

HostTexture *SurfaceImageForDraw(VideoState &s, GuestSurface &surf, u32 samples,
                                 bool writes_color = true);

HostTexture *SurfaceDepthSingle(VideoState &s, GuestSurface &surf, bool refresh);

bool SurfacePropagateDepthSingle(VideoState &s, GuestSurface &surf);

HostTexture *SurfaceColorSingle(VideoState &s, GuestSurface &surf);
HostTexture *SurfaceContentPeek(VideoState &s, GuestSurface &surf);

void NoteSurfaceDrawn(GuestSurface &surf, const HostTexture &image, bool writes_depth_stencil,
                      bool writes_depth = false);
void NoteSurfaceClearedColor(GuestSurface &surf, const HostTexture &image, const float rgba[4],
                             bool whole, bool both);
void NoteSurfaceClearedDepth(GuestSurface &surf, float depth, u8 stencil, bool whole, bool both);
inline bool SurfaceHasSingle(const GuestSurface &surf) { return surf.single.valid(); }
inline bool SurfaceIsMultisampled(const GuestSurface &surf) { return surf.host.sampleCount > 1; }
inline bool SurfaceHasImage(const GuestSurface &surf) { return surf.host.valid() || surf.single.valid(); }
bool SurfaceMakeMultisampled(VideoState &s, GuestSurface &surf);

void DestroySurfaceImages(VideoState &s, GuestSurface &surf);

void SurfaceTransferToMirror(VideoState &s, GuestSurface &surf, HostTexture &src,
                             GuestTexture &target, const std::shared_ptr<GuestTexture> &target_ref);
bool SurfaceTakeBack(VideoState &s, GuestSurface &surf);
void SurfaceRedirectBegin(VideoState &s, GuestSurface &surf);
bool SurfaceRedirectEnd(VideoState &s, GuestSurface &surf);
void SurfacesReleaseMirror(VideoState &s, const GuestTexture &t);
void TextureReleaseBorrower(VideoState &s, GuestTexture &t);

bool FlushAliasCopy(VideoState &s, GuestTexture &t);

void FlushAliasDependents(VideoState &s, GuestTexture &src);

}
