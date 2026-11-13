// gpu/d3d.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <atomic>
#include <bit>

#include <rex/types.h>

#include "core/memory_helpers.h"

namespace eot::gpu {

namespace dev {

constexpr u32 kDeviceSize = 0x6080;

constexpr u32 kPendingMask = 0x000;
constexpr u32 kFetchConstants = 0x480;
constexpr u32 kVsFloatConstants = 0x780;
constexpr u32 kPsFloatConstants = 0x1780;
constexpr u32 kVsBoolConstants = 0x2780;
constexpr u32 kPsBoolConstants = 0x2790;
constexpr u32 kVsLoopConstants = 0x27A0;
constexpr u32 kPsLoopConstants = 0x27E0;

constexpr u32 kSurfaceInfo = 0x2880;
constexpr u32 kColor0Info = 0x2884;
constexpr u32 kDepthInfo = 0x2888;
constexpr u32 kColor1Info = 0x288C;
constexpr u32 kColor2Info = 0x2890;
constexpr u32 kColor3Info = 0x2894;
constexpr u32 kScreenScissorTL = 0x28B8;
constexpr u32 kScreenScissorBR = 0x28BC;
constexpr u32 kWindowOffset = 0x28C0;
constexpr u32 kWindowScissorTL = 0x28C4;
constexpr u32 kWindowScissorBR = 0x28C8;
constexpr u32 kColorMask = 0x28DC;
constexpr u32 kBlendRed = 0x28E0;
constexpr u32 kStencilRefMaskBF = 0x28FC;
constexpr u32 kStencilRefMask = 0x2900;
constexpr u32 kAlphaRef = 0x2904;
constexpr u32 kVportXScale = 0x2908;
constexpr u32 kVportXOffset = 0x290C;
constexpr u32 kVportYScale = 0x2910;
constexpr u32 kVportYOffset = 0x2914;
constexpr u32 kVportZScale = 0x2918;
constexpr u32 kVportZOffset = 0x291C;
constexpr u32 kProgramControl = 0x2920;
constexpr u32 kDepthControl = 0x2934;
constexpr u32 kBlendControl0 = 0x2938;
constexpr u32 kColorControl = 0x293C;
constexpr u32 kHiControl = 0x2940;
constexpr u32 kClipControl = 0x2944;
constexpr u32 kModeControl = 0x2948;
constexpr u32 kVteControl = 0x294C;
constexpr u32 kEdramModeControl = 0x2954;
constexpr u32 kBlendControl1 = 0x2958;
constexpr u32 kBlendControl2 = 0x295C;
constexpr u32 kBlendControl3 = 0x2960;
constexpr u32 kAaConfig = 0x29BC;
constexpr u32 kVtxControl = 0x29C0;
constexpr u32 kCopyControl = 0x2A18;
constexpr u32 kCopyDestBase = 0x2A1C;
constexpr u32 kCopyDestPitch = 0x2A20;
constexpr u32 kCopyDestInfo = 0x2A24;
constexpr u32 kPolyOffsetFrontScale = 0x2A50;
constexpr u32 kPolyOffsetFrontOffset = 0x2A54;
constexpr u32 kPolyOffsetBackScale = 0x2A58;
constexpr u32 kPolyOffsetBackOffset = 0x2A5C;

// WoS port: Web of Shadows' D3D keeps its object pointers 0x160 lower than Edge of
// Time's, and its vertex declaration elsewhere; everything above (constants, the
// register shadow) is where Edge of Time has it. Each verified in WoS's own code
// (wos/NATIVE-RENDERER-NOTES.md, P2): SetVertexDeclaration sub_82AB4630 stores 0x2E2C,
// SetIndices 0x3094, SetRenderTarget 0x3098 + 4i, SetDepthStencilSurface 0x30A8,
// SetStreamSource 0x30AC + 4i / stride byte 0x30F0 + i, SetTexture 0x3100 + 4i,
// SetPixelShader 0x3194, SetVertexShader 0x3198.
constexpr u32 kVertexDeclaration = 0x2E2C;  // EoT 0x2FB8
constexpr u32 kIndexBuffer = 0x3094;        // EoT 0x31F4
constexpr u32 kRenderTarget0 = 0x3098;      // EoT 0x31F8
constexpr u32 kDepthSurface = 0x30A8;       // EoT 0x3208
constexpr u32 kStreamObject0 = 0x30AC;      // EoT 0x320C
constexpr u32 kStreamStride0 = 0x30F0;      // EoT 0x3250
constexpr u32 kTextureObject0 = 0x3100;     // EoT 0x3260
constexpr u32 kPixelShader = 0x3194;        // EoT 0x32F4
constexpr u32 kVertexShader = 0x3198;       // EoT 0x32F8

inline u32 StreamFetchSlotOffset(u32 stream) {
  return kFetchConstants + 8u * (95u - stream);
}

}

namespace obj {

constexpr u32 kCommon = 0x00;
constexpr u32 kReferenceCount = 0x04;
constexpr u32 kBaseFlush = 0x14;

constexpr u32 kTextureMipFlush = 0x18;
constexpr u32 kTextureFetch = 0x1C;

constexpr u32 kSurfaceInfo = 0x18;
constexpr u32 kSurfaceColorInfo = 0x1C;
constexpr u32 kSurfaceHiControl = 0x20;
constexpr u32 kSurfaceSize = 0x24;
constexpr u32 kSurfaceFormat = 0x28;   // D3DFORMAT dword; low 6 bits = texture format

constexpr u32 kBufferFetch0 = 0x18;
constexpr u32 kBufferFetch1 = 0x1C;
constexpr u32 kIndexBuffer32BitBit = 0x80000000u;

constexpr u32 kVertexShaderPhysical = 0x20;
constexpr u32 kVertexShaderContainer = 0x368;
constexpr u32 kPixelShaderPhysical = 0x18;
constexpr u32 kPixelShaderContainer = 0x28;

constexpr u32 kDeclElementCount = 0x18;
constexpr u32 kDeclMaxStream = 0x1C;
constexpr u32 kDeclElements = 0x34;

}

struct ShaderContainerHeader {
  be_u32 flags;          // 0x102A11xx, bit 0 = vertex shader
  be_u32 virtualSize;
  be_u32 physicalSize;
  be_u32 fieldC;
  be_u32 constantTableOffset;
  be_u32 definitionTableOffset;
  be_u32 shaderOffset;
  be_u32 field1C;
  be_u32 field20;
};
static_assert(sizeof(ShaderContainerHeader) == 0x24);

struct ShaderRecord {
  be_u32 physicalOffset;
  be_u32 size;
  be_u32 field8;
  be_u32 fieldC;
  be_u32 field10;
  be_u32 interpolatorInfo;
};

struct VertexShaderRecord : ShaderRecord {
  be_u32 field18;
  be_u32 vertexElementCount;
  be_u32 field20;
  be_u32 vertexElementsAndInterpolators[1];
};

struct DeclElement {
  be_u16 stream;
  be_u16 offset;
  be_u32 type;
  u8 method;
  u8 usage;
  u8 usageIndex;
  u8 pad;
};
static_assert(sizeof(DeclElement) == 12);

enum class DeclUsage : u8 {
  Position = 0,
  BlendWeight = 1,
  BlendIndices = 2,
  Normal = 3,
  PointSize = 4,
  TexCoord = 5,
  Tangent = 6,
  Binormal = 7,
  TessFactor = 8,
  PositionT = 9,
  Color = 10,
  Fog = 11,
  Depth = 12,
  Sample = 13,
};

constexpr u32 kDevicePageShift = 5;
inline std::atomic<u32> g_device_block_misses{0};
inline std::atomic<u32> g_device_block_miss_offset{0};

struct DeviceView {
  u32 va = 0;
  const u8 *snapshot = nullptr;
  u32 snapshotSize = 0;
  const bool *pages = nullptr;
  const u8 *Resolve(u32 off, u32 bytes) const {
    if (!snapshot || off + bytes > snapshotSize)
      return nullptr;
    if (pages && !(pages[off >> kDevicePageShift] && pages[(off + bytes - 1) >> kDevicePageShift])) {
      // WoS port (fix51): a plain load/store, not a locked increment - ~20 misses a draw
      // (the vertex shader pointer at 0x3198); the count is a diagnostic
      const u32 misses = g_device_block_misses.load(std::memory_order_relaxed);
      g_device_block_misses.store(misses + 1, std::memory_order_relaxed);
      if (misses == 0)
        g_device_block_miss_offset.store(off, std::memory_order_relaxed);
      return nullptr;
    }
    return snapshot + off;
  }
  u32 U32(u32 off) const {
    if (const u8 *p = Resolve(off, 4))
      return static_cast<u32>(*reinterpret_cast<const be_u32 *>(p));
    return mem::load<u32>(va + off);
  }
  f32 F32(u32 off) const { return std::bit_cast<f32>(U32(off)); }
  u8 U8(u32 off) const {
    if (const u8 *p = Resolve(off, 1))
      return *p;
    return mem::load<u8>(va + off);
  }
  const u8 *Bytes(u32 off, u32 bytes) const {
    if (const u8 *p = Resolve(off, bytes))
      return p;
    return mem::at<u8>(va + off);
  }
  explicit operator bool() const { return va != 0; }
};

constexpr u32 kDeviceSnapshotBytes = 0x3400;

inline DeviceView Device(u32 device_va) { return DeviceView{device_va}; }

}
