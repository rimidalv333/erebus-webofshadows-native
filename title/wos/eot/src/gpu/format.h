// gpu/format.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <rex/graphics/xenos.h>
#include <rex/types.h>

#include <plume_render_interface.h>

namespace eot::gpu {

plume::RenderFormat ConvertColorRenderTargetFormat(u32 xenos_color_format);

inline plume::RenderFormat DepthRenderTargetFormat() {
  return plume::RenderFormat::D32_FLOAT_S8_UINT;
}

inline plume::RenderFormat ShadowDepthFormat() {
#if defined(EOT_D3D12)
  return plume::RenderFormat::D32_FLOAT;
#else
  return DepthRenderTargetFormat();
#endif
}

inline bool FormatHasStencil(plume::RenderFormat format) {
  return format == plume::RenderFormat::D32_FLOAT_S8_UINT;
}

inline plume::RenderFormat ImageResourceFormat(plume::RenderFormat format) {
#if defined(EOT_D3D12)
  if (format == plume::RenderFormat::D32_FLOAT)
    return plume::RenderFormat::R32_TYPELESS;
#endif
  return format;
}

plume::RenderFormat SampledViewFormat(plume::RenderFormat format);

inline bool IsDepthFormat(plume::RenderFormat format) {
  return format == plume::RenderFormat::D32_FLOAT ||
         format == plume::RenderFormat::D32_FLOAT_S8_UINT;
}

struct TextureFormatMapping {
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  plume::RenderFormat srgbFormat = plume::RenderFormat::UNKNOWN;
  bool supported = false;
  bool blockCompressed = false;
  bool convert = false;
};
TextureFormatMapping MapTextureFormat(rex::graphics::xenos::TextureFormat format);

plume::RenderFormat ResolveDestinationFormat(rex::graphics::xenos::TextureFormat format,
                                             bool depth_source);

plume::RenderBlend ConvertBlendFactor(u32 value);
plume::RenderBlendOperation ConvertBlendOp(u32 value);
plume::RenderComparisonFunction ConvertCompareFunc(u32 value);
plume::RenderStencilOp ConvertStencilOp(u32 value);
plume::RenderPrimitiveTopology ConvertPrimitiveType(u32 xenos_prim, bool *expand_needed);

struct DeclTypeInfo {
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  u32 xenosFormat = 0;
  bool signedData = false;
  bool integer = false;
  bool sixteenBit = false;
  bool packedDec3 = false;
  bool packed111110 = false;
  u32 byteSize = 0;
};
DeclTypeInfo DecodeDeclType(u32 decl_type);

const char *DeclUsageSemantic(u32 usage);

}
