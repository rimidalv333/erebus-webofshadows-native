// gpu/textures.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/resources.h"

namespace eot::gpu {

struct VideoState;

// WoS port: fallback_fetch (host-order words, e.g. the draw's device fetch constant) is
// used when the header in guest memory no longer holds a texture fetch constant: Web of
// Shadows builds some texture headers on the stack (0x70xxxxxx), gone by replay time.
GuestTexture *GetGuestTexture(VideoState &s, u32 header_va, bool create_host_image = true,
                              const u32 *fallback_fetch = nullptr);

u32 PrepareTextureForSampling(VideoState &s, GuestTexture &t,
                              u32 swizzle = kIdentityFetchSwizzle);
u32 SamplingSwizzle(const GuestTexture &t, u32 fetch_swizzle);

bool EnsureResolveMirror(VideoState &s, GuestTexture &t, bool depth_source, float scale = 0.0f,
                         plume::RenderFormat depth_format = plume::RenderFormat::UNKNOWN);

void NoteResolveDestination(VideoState &s, const GuestTexture &t);

void PreloadAnnouncedTextures(VideoState &s, f64 budget_ms);

plume::RenderFramebuffer *GetMipFramebuffer(VideoState &s, GuestTexture &t, u32 mip);

void NotifyResourceUnlocked(u32 resource_va);
void NotifyResourceUnlockedLocked(VideoState &s, u32 resource_va);
u64 ResourceUnlockSeq(u32 resource_va);

}

namespace eot::gpu {

plume::RenderSamplerDesc DecodeSamplerFromFetch(const u32 fc[6], bool mipmapped_upload = true,
                                                bool synthesized_mips = false);

u32 ResolveSamplerSlotLocked(const plume::RenderSamplerDesc &desc);

}
