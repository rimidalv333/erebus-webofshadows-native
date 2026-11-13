// gpu/backend.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

namespace eot::gpu {

#if defined(EOT_D3D12)
inline constexpr bool g_vulkan = false;
inline constexpr plume::RenderShaderFormat kHostShaderFormat =
    plume::RenderShaderFormat::DXIL;
#define EOT_BLOB_SYMBOL(name) g_##name##_dxil
#else
inline constexpr bool g_vulkan = true;
inline constexpr plume::RenderShaderFormat kHostShaderFormat =
    plume::RenderShaderFormat::SPIRV;
#define EOT_BLOB_SYMBOL(name) g_##name##_spirv
#endif

#if defined(EOT_MVK)
inline constexpr bool g_mvk = true;
static_assert(g_vulkan);
#else
inline constexpr bool g_mvk = false;
#endif

#define EOT_SHADER_BLOB(name) EOT_BLOB_SYMBOL(name), sizeof(EOT_BLOB_SYMBOL(name))

#if defined(EOT_D3D12)
inline constexpr u32 kCopyPushConstantRangeIndex = 0;
inline constexpr u32 kCopyPushConstantByteOffset = 0;
inline constexpr u32 kSpecPushConstantRangeIndex = 1;
#else
inline constexpr u32 kGuestPushConstantRangeIndex = 0;
inline constexpr u32 kCopyPushConstantRangeIndex = 0;
inline constexpr u32 kCopyPushConstantByteOffset = 24;
#endif

}
