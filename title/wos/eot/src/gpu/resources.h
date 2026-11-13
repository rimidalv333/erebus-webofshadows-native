// gpu/resources.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/shaders/shader_cache.h"

namespace eot::gpu {

constexpr u32 kInvalidDescriptorIndex = ~u32{0};

enum : u32 {
  kNullTexture2DDescriptorIndex = 0,
  kNullTexture3DDescriptorIndex = 1,
  kNullTextureCubeDescriptorIndex = 2,
  kNullTextureDescriptorCount = 3,
};

constexpr u32 kIdentityFetchSwizzle = 0x688;

struct HostTexture {
  std::unique_ptr<plume::RenderTexture> texture;
  plume::RenderTextureDesc desc;
  std::unique_ptr<plume::RenderTextureView> srv;
  u32 descriptorIndex = kInvalidDescriptorIndex;
  u32 stencilDescriptorIndex = kInvalidDescriptorIndex;
  struct SwizzledSrv {
    std::unique_ptr<plume::RenderTextureView> view;
    u32 descriptorIndex = kInvalidDescriptorIndex;
  };
  std::unordered_map<u32, SwizzledSrv> swizzledSrvs;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  plume::RenderTextureViewDimension viewDimension =
      plume::RenderTextureViewDimension::TEXTURE_2D;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 mipLevels = 1;
  u32 arraySize = 1;
  u32 sampleCount = 1;
  bool isDepth = false;
  bool renderable = false;
  bool needsClear = false;
  std::vector<std::unique_ptr<plume::RenderTextureView>> mipViews;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> mipFramebuffers;
  std::unique_ptr<plume::RenderTextureView> depthView;
  bool transientPool = false;

  bool valid() const { return texture != nullptr; }
};

struct GuestSurface;

struct VelocityHandle {
  u64 depthUid = 0;
  u32 slot = 0;
  u64 generation = 0;
  u64 frame = ~0ull;
};

struct ResolvePacket;

struct GuestTexture {
  u32 va = 0;
  u32 fetch[6] = {};
  rex::graphics::xenos::TextureFormat format =
      rex::graphics::xenos::TextureFormat::k_8_8_8_8;
  rex::graphics::xenos::DataDimension dimension =
      rex::graphics::xenos::DataDimension::k2DOrStacked;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 mipLevels = 1;
  bool tiled = false;
  u32 baseAddress = 0;
  u32 mipAddress = 0;
  bool gammaSigned = false;

  HostTexture host;

  u64 bindingGeneration = 1;

  bool uploaded = false;
  u64 uploadedUnlockSeq = 0;
  bool uploadFailed = false;
  bool synthMips = false;
  bool resolveOwned = false;
  bool resolveProvisional = false;
  u64 ownerScanEpoch = 0;
  u64 contentHash = 0;        // WoS port: sparse hash of the guest bytes last uploaded
  u64 contentCheckFrame = 0;  // WoS port: guest frame the hash was last compared in
  u64 provisionalHash = 0;    // WoS port (fix82): guest bytes' hash when taken over as a resolve target
  // WoS port (fix83): full hashes of 32 slices of the guest bytes (base + mips), one slice
  // re-checked per frame - catches partial rewrites the sparse contentHash misses
  u64 sliceHash[32] = {};
  bool slicesValid = false;
  u32 contentChanges = 0;     // WoS port: re-uploads because the bytes changed under it  // WoS port: resolve-owner epoch this slot last looked for an owner at
  bool storeSwapRB = false;
  u64 contentSerial = 0;
  GuestSurface *borrower = nullptr;
  std::weak_ptr<GuestTexture> aliasSource;
  const plume::RenderTexture *aliasSourceImage = nullptr;
  bool aliasPending = false;
  std::vector<std::weak_ptr<GuestTexture>> aliasDependents;
  u64 resolvedSurfaceUid = 0;
  u64 resolvedSurfaceSerial = 0;
  u64 resolvedOwnSerial = 0;
  u32 resolvedLevel = 0;
  i32 resolvedRect[6] = {0, 0, 0, 0, 0, 0};
  u32 resolvedMipMask = 0;
  VelocityHandle velocity;
  u32 resolveOrdinal = 0;
  u64 resolveOrdinalFrame = ~0ull;
  u32 handoffRegretMask = 0;
  u64 handoffRegretResetFrame = 0;
  u64 lastUseFrame = 0;
  u64 lastSampledFrame = 0;
  u64 lastResolvedFrame = 0;
  u64 resolveWriteSeq = 0;  // WoS port (fix101): VideoState::resolve_write_seq of its last resolve
  u64 aliasVisitToken = 0;
  u64 perfSamples = 0;
  u64 perfResolves = 0;
  u64 perfDeadResolves = 0;
  std::shared_ptr<ResolvePacket> lastResolve;
};

struct GuestSurface {
  u32 va = 0;
  u32 surfaceInfo = 0; // +0x18
  u32 info = 0;        // +0x1C  RB_COLOR_INFO / RB_DEPTH_INFO word
  u32 hiControl = 0;   // +0x20
  u32 sizeBits = 0;    // +0x24
  u32 formatWord = 0;  // +0x28
  u32 width = 0;
  u32 height = 0;
  u32 msaaSamples = 1;
  bool isDepth = false;
  u32 colorFormat = 0;
  u32 depthFormat = 0;
  u32 baseTile = 0;
  i32 colorExpBias = 0;
  float scale = 1.0f;
  u32 allocWidth = 0;
  u32 allocHeight = 0;

  HostTexture host;
  HostTexture single;
  bool contentInSingle = false;
  bool imagesAgree = true;
  bool resolvedSinceDraw = false;
  enum class Content : u8 { Undefined, Cleared, Drawn, Borrowed };
  Content content = Content::Undefined;
  std::weak_ptr<GuestTexture> borrowed;
  u64 borrowedSerial = 0;
  u64 uid = 0;
  u64 serial = 1;
  u64 wholeClearSerial = 0;
  u64 handoffFrame = ~0ull;
  std::weak_ptr<GuestTexture> handoffMirror;
  u32 handoffMirrorOrdinal = 0;
  struct RedirectPrediction {
    std::weak_ptr<GuestTexture> mirror;
    const void *texture = nullptr;
    i32 x = 0, y = 0;
    u32 streak = 0;
    u64 recordedFrame = ~0ull;
  };
  static constexpr u32 kRedirectPasses = 8;
  RedirectPrediction redirectPredictions[kRedirectPasses];
  u32 redirectPasses = 0;
  u64 redirectPassFrame = ~0ull;
  std::shared_ptr<GuestTexture> redirectMirror;
  i32 redirectX = 0, redirectY = 0;
  float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float clearDepth = 0.0f;
  u8 clearStencil = 0;
  u64 writeSerial = 1;
  u64 singleSerial = 0;
  bool singleDirty = false;
  bool drawn = false;
  u64 lastUseFrame = 0;
  u64 perfDraws = 0;
  u64 perfClears = 0;
  u64 perfResolves = 0;
  u64 perfTransfers = 0;
  u64 hostDraws = 0;
  u64 singleDraws = 0;
  u64 createdFrame = 0;
};

struct VertexInput {
  u8 usage = 0;
  u8 usageIndex = 0;
  u16 address = 0; // instruction address of its vfetch
  // WoS port (fix55): what the shader's own vfetch instruction reads. The GPU fetches by
  // these (offset/format/sign from the instruction), not by the declaration's usage names.
  bool hasFetch = false;
  u8 fetchFormat = 0; // xenos vertex format (k_16_16_16_16 = 26, ...)
  bool fetchSigned = false;
  bool fetchInteger = false;
  u8 fetchStream = 0;  // 95 - fetch constant index
  u32 fetchOffset = 0; // bytes
  u32 fetchStride = 0; // bytes
};

struct InputLayout;

struct GuestShader {
  u32 va = 0;
  u64 hash = 0;
  bool isPixel = false;
  const ShaderCacheEntry *entry = nullptr;
  bool cacheMissLogged = false;
  bool createdByGuestCall = false;
  std::vector<VertexInput> inputs;
  bool usesFloatConstants = true;
  u32 textureFetchMask = 0xFFFFu;
  u32 floatConstantRegs = 256;
  u32 lastSpecMask = ~0u;
  u8 lastVariant = 0;
  plume::RenderShader *lastHost = nullptr;
  u32 lastDeclVa = 0;
  u32 lastDeclCount = 0;
  const InputLayout *lastLayout = nullptr;
  u8 lastDeclRaw[32 * 12] = {};
  // WoS port (fix56): where the microcode lives; D3D patches the vfetches in place when it
  // binds the shader to a declaration, so they are read live when a layout is built
  u32 codeVa = 0;
  u32 codeSize = 0;
  u32 unpatchedTries = 0;
};

}
