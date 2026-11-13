// gpu/shaders/guest_shaders.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <vector>

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/resources.h"

struct ShaderCacheEntry;

namespace eot::gpu {

struct VideoState;

constexpr u32 kSpecR11G11B10Normal = 1u << 0;
constexpr u32 kSpecAlphaTest = 1u << 1;
constexpr u32 kSpecSintTexcoord = 1u << 2;

bool GuestShadersInit();
u32 GuestShaderCacheCount();

GuestShader *RegisterGuestShader(VideoState &s, u32 object_va, bool is_pixel);
GuestShader *FindGuestShader(VideoState &s, u32 object_va);
void DrainShaderGraveyardLocked(VideoState &s);

const ShaderCacheEntry *FindShaderCacheEntry(u64 hash);
u64 CanonicalShaderHash(u64 hash);
void VertexInputsFromEntry(const ShaderCacheEntry &e, std::vector<VertexInput> &out);
// WoS port (fix56): the shader's inputs with what its (D3D-patched) vfetches read right now:
// 1 all decoded, 0 none/undecodable (name binding), -1 not patched yet
int LiveFetchInputs(const GuestShader &vs, std::vector<VertexInput> &out);
enum class VsVariant : u8 { Trimmed = 0, Full = 1, PositionOnly = 2, Velocity = 3 };
VsVariant VsVariantFor(const ShaderCacheEntry *vs, const ShaderCacheEntry *ps, bool null_ps,
                       bool velocity = false);
bool EntryHasVelocity(const ShaderCacheEntry *e);

plume::RenderShader *GetHostShaderByHash(VideoState &s, u64 hash, u32 spec_mask, bool is_pixel,
                                         bool worker = false,
                                         VsVariant variant = VsVariant::Trimmed);

plume::RenderShader *ResolveHostShader(VideoState &s, GuestShader &shader, u32 spec_mask,
                                       VsVariant variant = VsVariant::Trimmed);

}
