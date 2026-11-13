// gpu/surfaces.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <format>
#include "gpu/surfaces.h"
#include "gpu/wos_tiling.h"

#include <memory>

#include <rex/graphics/xenos.h>
#include <rex/memory/utils.h>

#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#endif

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/gpu_timing.h"
#include "gpu/memory_report.h"
#include "gpu/settings.h"

namespace eot::gpu {

namespace {

namespace xe = rex::graphics::xenos;

bool IsDepthFormatWord(u32 format_word) {
  const u32 f = format_word & 0x3F;
  return f == static_cast<u32>(xe::TextureFormat::k_24_8) ||
         f == static_cast<u32>(xe::TextureFormat::k_24_8_FLOAT);
}

bool DecodeHeaderWords(u32 va, const u32 words[5], GuestSurface &out) {
  const u32 surface_info = words[0];
  const u32 info = words[1];
  const u32 hi = words[2];
  const u32 size_bits = words[3];
  const u32 format_word = words[4];
  const u32 width = (size_bits >> 18) + 1;
  const u32 height = ((size_bits >> 3) & 0x7FFF) + 1;
  if (width == 0 || height == 0 || width > 8192 || height > 8192)
    return false;
  out.va = va;
  out.surfaceInfo = surface_info;
  out.info = info;
  out.hiControl = hi;
  out.sizeBits = size_bits;
  out.formatWord = format_word;
  out.width = width;
  out.height = height;
  const u32 msaa = (surface_info >> 16) & 3;
  out.msaaSamples = msaa == 2 ? 4 : msaa == 1 ? 2 : 1;
  out.isDepth = IsDepthFormatWord(format_word);
  out.baseTile = info & 0xFFF;
  if (out.isDepth) {
    out.depthFormat = (info >> 16) & 1;
    out.colorFormat = 0;
    out.colorExpBias = 0;
  } else {
    out.colorFormat = (info >> 16) & 0xF;
    out.depthFormat = 0;
    const u32 bias = (info >> 20) & 0x3F;
    out.colorExpBias = bias & 0x20 ? static_cast<i32>(bias) - 64 : static_cast<i32>(bias);
  }
  return true;
}

bool ShadowTileShape(const GuestSurface &surf) {
  return surf.isDepth && surf.width == 1024 && surf.height == 1024;
}

u32 HostSampleCountFor(const VideoState &s, const GuestSurface &surf) {
  if (s.host_msaa_samples <= 1 || surf.msaaSamples != 1)
    return 1;
  if (surf.width != kGuestRenderWidth || surf.height != kGuestRenderHeight)
    return 1;
  return s.host_msaa_samples;
}

constexpr u32 kTextureCameraWideFrom = kGuestRenderWidth * 4 / 5;
static bool IsTextureCameraShape(const GuestSurface &surf) {
  if (surf.msaaSamples > 1 || surf.width <= 256 || surf.height <= 256)
    return false;
  if (surf.isDepth && surf.width == 1024 && surf.height == 1024)
    return false;
  for (u32 k = 0; k < 4; ++k)
    if (surf.width == (kGuestRenderWidth >> k) && surf.height == (kGuestRenderHeight >> k))
      return false;
  return true;
}

static float TextureCameraScale(bool wide) {
  const float base = RenderScaleFactor();
  const float pip = static_cast<float>(std::clamp(Settings::PipScalePercent(), 25, 100)) / 100.0f;
  return std::max(1.0f, wide ? base : base * pip);
}

static bool TextureCameraWide(const GuestSurface &surf) {
  return surf.width > kTextureCameraWideFrom || TextureCameraScale(false) >= TextureCameraScale(true);
}

static void HostAllocationSize(const GuestSurface &surf, u32 &w, u32 &h) {
  w = surf.width;
  h = surf.height;
  if ((surf.width == kGuestRenderWidth && surf.height == kGuestRenderHeight) ||
      (surf.isDepth && surf.width == 1024 && surf.height == 1024))
    return;
  w = (w + 79u) / 80u * 80u;
  h = (h + 63u) / 64u * 64u;
  if (IsTextureCameraShape(surf)) {
    const u32 bucket_width = TextureCameraWide(surf) ? kGuestRenderWidth : kTextureCameraWideFrom;
    w = std::max(w, (bucket_width + 79u) / 80u * 80u);
    h = std::max(h, (kGuestRenderHeight + 63u) / 64u * 64u);
  }
}

HostTexture DescribeSurfaceImage(const GuestSurface &surf, u32 samples) {
  HostTexture host;
  host.format = SurfaceHostFormat(surf);
  host.width = ScaleDimBy(surf.allocWidth, surf.scale);
  host.height = ScaleDimBy(surf.allocHeight, surf.scale);
  host.depth = 1;
  host.mipLevels = 1;
  host.arraySize = 1;
  host.sampleCount = samples;
  host.isDepth = surf.isDepth;
  host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  return host;
}

bool CreateSurfaceImage(VideoState &s, GuestSurface &surf, HostTexture &host, u32 samples,
                        const char *tag) {
  host = DescribeSurfaceImage(surf, samples);

  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = host.width;
  desc.height = host.height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = ImageResourceFormat(host.format);
  desc.flags = surf.isDepth ? plume::RenderTextureFlag::DEPTH_TARGET
                            : plume::RenderTextureFlag::RENDER_TARGET;
  desc.multisampling.sampleCount = static_cast<plume::RenderSampleCounts>(host.sampleCount);
  desc.committed = u64(desc.width) * desc.height >= 256ull * 256ull;
  plume::RenderClearValue clear;
  if (!surf.isDepth) {
    clear = plume::RenderClearValue::Color(plume::RenderColor(0, 0, 0, 0), host.format);
    desc.optimizedClearValue = &clear;
  }
  host.texture = CreateHostTexture(s.device.get(), desc, tag);
  host.desc = desc;
  host.desc.optimizedClearValue = nullptr;
  host.layout = plume::RenderTextureLayout::UNKNOWN;
  host.needsClear = host.texture != nullptr;
  host.renderable = host.texture != nullptr;
  return host.texture != nullptr;
}

float SurfaceRenderScale(const GuestSurface &surf) {
  const float base = RenderScaleFactor();
  if (surf.isDepth && surf.width == 1024 && surf.height == 1024)
    return ShadowMapTargetScale();
  // WoS port: small targets render at 1x. Web of Shadows' bloom and eye-adaptation chain
  // goes down to 4x2, 2x1 and 1x1 targets; at a fractional scale (1.5 for 1080p) a 1x1
  // viewport covers one host pixel of a 2x2 image and the other three keep whatever the
  // recycled memory held - sampled back, that garbage (NaN, huge half floats) blew the
  // whole frame to white or a single colour, changing every frame.
  if (surf.width <= 256 || surf.height <= 256)
    return 1.0f;
  if (!surf.isDepth && surf.baseTile == 0 && surf.colorFormat == 1 &&
      surf.width == kGuestRenderWidth / 2 && surf.height == kGuestRenderHeight / 2)
    return std::max(1.0f, base * 0.5f);
  for (u32 k = 0; k < 4; ++k)
    if (surf.width == (kGuestRenderWidth >> k) && surf.height == (kGuestRenderHeight >> k))
      return base;
  return TextureCameraScale(TextureCameraWide(surf));
}

bool CreateHostTarget(VideoState &s, GuestSurface &surf) {
  HostAllocationSize(surf, surf.allocWidth, surf.allocHeight);
  surf.scale = SurfaceRenderScale(surf);
  surf.single = HostTexture{};
  surf.contentInSingle = false;
  surf.content = GuestSurface::Content::Undefined;
  surf.writeSerial = 1;
  surf.singleSerial = 0;
  static u64 next_uid = 1;
  surf.uid = next_uid++;
  surf.serial++;
  surf.wholeClearSerial = surf.serial;
  surf.createdFrame = s.guest_frames;
  const u32 samples = HostSampleCountFor(s, surf);
  if (samples <= 1)
    return CreateSurfaceImage(s, surf, surf.host, samples, surf.isDepth ? "surface-ds" : "surface-rt");
  if (!CreateSurfaceImage(s, surf, surf.single, 1, surf.isDepth ? "surface-ds-1x" : "surface-rt-1x"))
    return false;
  surf.host = DescribeSurfaceImage(surf, samples);
  surf.imagesAgree = true;
  surf.singleSerial = surf.writeSerial;
  return true;
}

}

bool IsTextureCameraSurface(const GuestSurface &surface) { return IsTextureCameraShape(surface); }

bool IsShadowTile(const GuestSurface &surface) { return ShadowTileShape(surface); }

plume::RenderFormat SurfaceHostFormat(const GuestSurface &surface) {
  if (surface.isDepth)
    return ShadowTileShape(surface) ? ShadowDepthFormat() : DepthRenderTargetFormat();
  return ConvertColorRenderTargetFormat(surface.colorFormat);
}

const plume::RenderTextureView *DepthTargetView(VideoState &s, HostTexture &host) {
  if (!host.isDepth || !host.texture || ImageResourceFormat(host.format) == host.format)
    return nullptr;
  if (!host.depthView) {
    plume::RenderTextureViewDesc vd;
    vd.format = host.format;
    vd.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    vd.mipSlice = 0;
    vd.mipLevels = 1;
    vd.arrayIndex = 0;
    vd.arraySize = 1;
    s.perf.host_views++;
    host.depthView = host.texture->createTextureView(vd);
  }
  return host.depthView.get();
}

u64 DescriptorKey(const GuestSurface &d) {
  u64 k = d.baseTile;
  k = k * 0x9E3779B97F4A7C15ull ^ d.allocWidth;
  k = k * 0x9E3779B97F4A7C15ull ^ d.allocHeight;
  k = k * 0x9E3779B97F4A7C15ull ^ (d.isDepth ? 0x100u : 0u);
  k = k * 0x9E3779B97F4A7C15ull ^ (d.isDepth ? d.depthFormat : d.colorFormat);
  k = k * 0x9E3779B97F4A7C15ull ^ d.msaaSamples;
  return k;
}

static bool HalvesTo(u32 full, u32 half) {
  return half && (half == full / 2 || half == (full + 1) / 2);
}

GuestSurface *FindMultisampleAliasSource(VideoState &s, const GuestSurface &alias) {
  if (alias.msaaSamples < 2 || alias.drawn || !alias.width || !alias.height)
    return nullptr;
  GuestSurface *best = nullptr;
  for (auto &[key, slot] : s.surfaces) {
    GuestSurface *c = slot.get();
    if (!c || static_cast<const GuestSurface *>(c) == &alias || !c->drawn || !SurfaceHasImage(*c))
      continue;
    if (c->msaaSamples != 1 || c->isDepth != alias.isDepth || c->baseTile != alias.baseTile)
      continue;
    if (c->isDepth ? c->depthFormat != alias.depthFormat : c->colorFormat != alias.colorFormat)
      continue;
    if (!HalvesTo(c->width, alias.width) || !HalvesTo(c->height, alias.height))
      continue;
    if (!best || c->lastUseFrame > best->lastUseFrame)
      best = c;
  }
  return best;
}

namespace {
struct SurfaceLookupEntry {
  u32 va = 0;
  u32 words[5] = {};
  u64 key = 0;
  u64 generation = 0;
  GuestSurface *surf = nullptr;
  u32 surfaceInfo = 0, info = 0, hiControl = 0, sizeBits = 0, width = 0, height = 0;
  i32 colorExpBias = 0;
};

void ApplyHeaderFields(GuestSurface &surf, const SurfaceLookupEntry &e) {
  surf.surfaceInfo = e.surfaceInfo;
  surf.info = e.info;
  surf.hiControl = e.hiControl;
  surf.sizeBits = e.sizeBits;
  surf.width = e.width;
  surf.height = e.height;
  surf.colorExpBias = e.colorExpBias;
}
SurfaceLookupEntry g_surface_lookup[16];
constexpr u32 kHeaderWordOffsets[5] = {obj::kSurfaceInfo, obj::kSurfaceColorInfo,
                                       obj::kSurfaceHiControl, obj::kSurfaceSize,
                                       obj::kSurfaceFormat};
}

bool ReadSurfaceHeaderWords(u32 surface_va, u32 words[5]) {
  const u8 *header = surface_va ? mem::at<u8>(surface_va) : nullptr;
  if (!header)
    return false;
  for (u32 i = 0; i < 5; ++i)
    words[i] = rex::memory::load_and_swap<u32>(header + kHeaderWordOffsets[i]);
  wos_tiling::ApplyExtent(surface_va, words);  // WoS port: predicated tiling
  return true;
}

// WoS port: predicated tiling (gpu/wos_tiling.h). The table only grows: a surface once
// tiled keeps its extent, so the game thread's captures and the render thread's reads
// of the same header always agree.
namespace wos_tiling {
namespace {
struct Tiled {
  std::atomic<u32> va{0};
  std::atomic<u32> extent{0};  // width << 16 | height
};
constexpr u32 kMaxTiled = 16;
Tiled g_tiled[kMaxTiled];
std::atomic<u32> g_tiled_count{0};
std::mutex g_tiled_add;
std::atomic<bool> g_clear_whole{false};
}  // namespace

void Begin(u32 device_va, u32 width, u32 height) {
  if (!device_va || !width || !height || width > 8192 || height > 8192)
    return;
  u32 vas[5];
  for (u32 i = 0; i < 4; ++i)
    vas[i] = mem::load<u32>(device_va + dev::kRenderTarget0 + 4 * i);
  vas[4] = mem::load<u32>(device_va + dev::kDepthSurface);
  std::lock_guard lock(g_tiled_add);
  for (u32 va : vas) {
    if (!va)
      continue;
    const u32 extent = (width << 16) | height;
    const u32 n = g_tiled_count.load(std::memory_order_acquire);
    bool found = false;
    for (u32 i = 0; i < n; ++i) {
      if (g_tiled[i].va.load(std::memory_order_relaxed) == va) {
        g_tiled[i].extent.store(extent, std::memory_order_release);
        found = true;
        break;
      }
    }
    if (found || n >= kMaxTiled)
      continue;
    g_tiled[n].extent.store(extent, std::memory_order_relaxed);
    g_tiled[n].va.store(va, std::memory_order_relaxed);
    g_tiled_count.store(n + 1, std::memory_order_release);
    EOT_INFO("[tiling] surface {:#x} drawn whole at {}x{} (predicated tiling)", va, width, height);
  }
}

void SetClearWhole(bool on) { g_clear_whole.store(on, std::memory_order_release); }
bool ClearWhole() { return g_clear_whole.load(std::memory_order_acquire); }

void ApplyExtent(u32 surface_va, u32 words[5]) {
  const u32 n = g_tiled_count.load(std::memory_order_acquire);
  for (u32 i = 0; i < n; ++i) {
    if (g_tiled[i].va.load(std::memory_order_relaxed) != surface_va)
      continue;
    const u32 extent = g_tiled[i].extent.load(std::memory_order_acquire);
    const u32 ew = extent >> 16, eh = extent & 0xFFFF;
    const u32 size = words[3];
    const u32 w = (size >> 18) + 1, h = ((size >> 3) & 0x7FFF) + 1;
    if (w > ew || h >= eh)
      return;  // already as large: not a tile-sized header
    words[3] = (size & ~((0x3FFFu << 18) | (0x7FFFu << 3))) | ((std::max(w, ew) - 1) << 18) |
               ((eh - 1) << 3);
    return;
  }
}
namespace {
struct DrawWords {
  std::atomic<u32> va{0};
  u32 words[5] = {};
};
DrawWords g_draw_words[kMaxTiled];
std::atomic<bool> g_end_tiling{false};
}  // namespace

void NoteDrawWords(u32 surface_va, const u32 words[5]) {
  const u32 n = g_tiled_count.load(std::memory_order_acquire);
  for (u32 i = 0; i < n; ++i) {
    if (g_tiled[i].va.load(std::memory_order_relaxed) != surface_va)
      continue;
    std::lock_guard lock(g_tiled_add);
    g_draw_words[i].va.store(surface_va, std::memory_order_relaxed);
    std::memcpy(g_draw_words[i].words, words, sizeof(g_draw_words[i].words));
    return;
  }
}

void UseDrawWords(u32 surface_va, u32 words[5]) {
  const u32 n = g_tiled_count.load(std::memory_order_acquire);
  for (u32 i = 0; i < n; ++i) {
    if (g_draw_words[i].va.load(std::memory_order_relaxed) != surface_va)
      continue;
    std::lock_guard lock(g_tiled_add);
    if (std::memcmp(words, g_draw_words[i].words, sizeof(g_draw_words[i].words)) != 0) {
      static std::atomic<u32> logged{0};
      if (logged.fetch_add(1) < 4)
        EOT_INFO("[tiling] resolve of {:#x}: header [{:08x} {:08x} {:08x} {:08x} {:08x}] -> the "
                 "drawn one [{:08x} {:08x} {:08x} {:08x} {:08x}]",
                 surface_va, words[0], words[1], words[2], words[3], words[4],
                 g_draw_words[i].words[0], g_draw_words[i].words[1], g_draw_words[i].words[2],
                 g_draw_words[i].words[3], g_draw_words[i].words[4]);
      std::memcpy(words, g_draw_words[i].words, sizeof(g_draw_words[i].words));
    }
    return;
  }
}

void SetEndTiling(bool on) { g_end_tiling.store(on, std::memory_order_release); }
bool InEndTiling() { return g_end_tiling.load(std::memory_order_acquire); }
}  // namespace wos_tiling

GuestSurface *GetGuestSurface(VideoState &s, u32 surface_va) {
  if (!surface_va)
    return nullptr;
  u32 words[5] = {};
  if (!ReadSurfaceHeaderWords(surface_va, words)) {
    u32 n;
    if (DiagShouldLog(0x5C00 ^ surface_va, &n))
      EOT_WARN("[surfaces] {:#x}: unreadable or absurd header", surface_va);
    return nullptr;
  }
  return GetGuestSurfaceWords(s, surface_va, words);
}

GuestSurface *GetGuestSurfaceWords(VideoState &s, u32 surface_va, const u32 words_in[5]) {
  if (!surface_va)
    return nullptr;
  SurfaceLookupEntry &lookup = g_surface_lookup[(surface_va >> 4) & 15];
  u32 words[5];
  std::memcpy(words, words_in, sizeof(words));
  const bool header = true;
  if (lookup.surf && lookup.va == surface_va && lookup.generation == s.surface_generation &&
      std::memcmp(lookup.words, words, sizeof(words)) == 0 && SurfaceHasImage(*lookup.surf)) {
    lookup.surf->va = surface_va;
    ApplyHeaderFields(*lookup.surf, lookup);
    return lookup.surf;
  }
  GuestSurface decoded;
  if (!DecodeHeaderWords(surface_va, words, decoded)) {
    u32 n;
    if (DiagShouldLog(0x5C00 ^ surface_va, &n))
      EOT_WARN("[surfaces] {:#x}: unreadable or absurd header", surface_va);
    return nullptr;
  }
  HostAllocationSize(decoded, decoded.allocWidth, decoded.allocHeight);
  const u64 key = DescriptorKey(decoded);
  auto &slot = s.surfaces[key];
  auto remember = [&](GuestSurface *surf) {
    lookup.va = surface_va;
    std::memcpy(lookup.words, words, sizeof(words));
    lookup.key = key;
    lookup.generation = s.surface_generation;
    lookup.surf = header ? surf : nullptr;
    lookup.surfaceInfo = surf->surfaceInfo;
    lookup.info = surf->info;
    lookup.hiControl = surf->hiControl;
    lookup.sizeBits = surf->sizeBits;
    lookup.width = surf->width;
    lookup.height = surf->height;
    lookup.colorExpBias = surf->colorExpBias;
    return surf;
  };
  if (slot && SurfaceHasImage(*slot)) {
    slot->va = surface_va;
    slot->surfaceInfo = decoded.surfaceInfo;
    slot->info = decoded.info;
    slot->hiControl = decoded.hiControl;
    slot->sizeBits = decoded.sizeBits;
    slot->width = decoded.width;
    slot->height = decoded.height;
    slot->colorExpBias = decoded.colorExpBias;
    return remember(slot.get());
  }
  if (slot) {
    DestroySurfaceImages(s, *slot);
    s.surface_generation++;
  }
  auto surf = std::make_unique<GuestSurface>();
  *surf = std::move(decoded);
  if (!CreateHostTarget(s, *surf)) {
    slot.reset();
    s.surfaces.erase(key);
    s.surface_generation++;
    return nullptr;
  }
  EOT_DEBUG("[surfaces] {:#x}: {} {}x{} fmt={} msaa={} tile={} -> host fmt {} {}x{}{}{}", surface_va,
           surf->isDepth ? "depth" : "color", surf->width, surf->height,
           surf->isDepth ? surf->depthFormat : surf->colorFormat, surf->msaaSamples,
           surf->baseTile, static_cast<u32>(surf->host.format), surf->host.width,
           surf->host.height,
           surf->allocWidth != surf->width || surf->allocHeight != surf->height
               ? std::format(" (allocated as {}x{})", surf->allocWidth, surf->allocHeight)
               : "",
           surf->host.sampleCount > 1 ? std::format(" {}x samples", surf->host.sampleCount) : "");
  slot = std::move(surf);
  return remember(slot.get());
}

plume::RenderFramebuffer *GetFramebuffer(VideoState &s, HostTexture *const color[4],
                                         u32 color_count, HostTexture *depth) {
  u64 key = 0x9E3779B97F4A7C15ull;
  const plume::RenderTexture *colors[4] = {};
  u32 n = 0;
  for (u32 i = 0; i < color_count && i < 4; ++i) {
    if (!color[i] || !color[i]->texture)
      break;
    colors[n++] = color[i]->texture.get();
    key ^= reinterpret_cast<u64>(colors[i]) * (i + 1) * 0x100000001B3ull;
    key = (key << 13) | (key >> 51);
  }
  const plume::RenderTexture *ds = depth && depth->texture ? depth->texture.get() : nullptr;
  key ^= reinterpret_cast<u64>(ds) * 0xC2B2AE3D27D4EB4Full;
  if (n == 0 && !ds)
    return nullptr;
  auto it = s.framebuffers.find(key);
  if (it != s.framebuffers.end())
    return it->second.fb.get();
  plume::RenderFramebufferDesc desc(n ? colors : nullptr, n, ds);
  if (ds)
    desc.depthAttachmentView = DepthTargetView(s, *depth);
  s.perf.host_framebuffers++;
  auto fb = s.device->createFramebuffer(desc);
  if (!fb) {
    EOT_ERROR("[surfaces] createFramebuffer failed ({} colour, depth={})", n, ds != nullptr);
    return nullptr;
  }
  auto *raw = fb.get();
  VideoState::CachedFramebuffer entry;
  entry.fb = std::move(fb);
  for (u32 i = 0; i < n; ++i)
    entry.attachments[entry.attachmentCount++] = colors[i];
  if (ds)
    entry.attachments[entry.attachmentCount++] = ds;
  s.framebuffers.emplace(key, std::move(entry));
  return raw;
}

namespace {

void BindHelperFramebuffer(VideoState &s, plume::RenderFramebuffer *fb) {
  s.command_list->setFramebuffer(fb);
  s.bound_framebuffer = fb;
  s.bound_draw_targets_valid = false;
}

void HelperPassDone(VideoState &s) {
  s.bound_pipeline = nullptr;
  s.bound_framebuffer = nullptr;
  s.bound_draw_targets_valid = false;
}

plume::RenderFramebuffer *ImageFramebuffer(VideoState &s, HostTexture &image) {
  HostTexture *colors[4] = {image.isDepth ? nullptr : &image, nullptr, nullptr, nullptr};
  return GetFramebuffer(s, colors, image.isDepth ? 0u : 1u, image.isDepth ? &image : nullptr);
}

bool HelperBlit(VideoState &s, HostTexture &src, HostTexture &dst, plume::RenderPipeline *pso,
                u32 second_descriptor = 0, const float *src_rect = nullptr) {
  if (!pso || !src.valid() || !dst.valid())
    return false;
  plume::RenderFramebuffer *fb = ImageFramebuffer(s, dst);
  if (!fb)
    return false;
  auto *cmd = s.command_list;
  const HostTextureTransition transitions[] = {
      {&src, plume::RenderTextureLayout::SHADER_READ},
      {&dst, dst.isDepth ? plume::RenderTextureLayout::DEPTH_WRITE
                         : plume::RenderTextureLayout::COLOR_WRITE}};
  TransitionManyLocked(s, transitions, 2);
  BindHelperFramebuffer(s, fb);
  cmd->setPipeline(pso);
  const plume::RenderViewport vp(0.0f, 0.0f, static_cast<float>(dst.width),
                                 static_cast<float>(dst.height), 0.0f, 1.0f);
  const plume::RenderRect sc(0, 0, static_cast<i32>(dst.width), static_cast<i32>(dst.height));
  cmd->setViewports(&vp, 1);
  cmd->setScissors(&sc, 1);
  CopyPushConstants pc;
  pc.resourceDescriptorIndex = BindTextureSRVLocked(s, src);
  pc.resourceDescriptorIndex2 = second_descriptor;
  pc.param0 = 1.0f;
  pc.param1 = 0.0f;
  if (src_rect)
    std::memcpy(pc.rect, src_rect, sizeof(pc.rect));
  cmd->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc, kCopyPushConstantByteOffset,
                                sizeof(pc));
  cmd->drawInstanced(3, 1, 0, 0);
  dst.needsClear = false;
  HelperPassDone(s);
  return true;
}

bool ClearImageToRemembered(VideoState &s, GuestSurface &surf, HostTexture &image) {
  plume::RenderFramebuffer *fb = ImageFramebuffer(s, image);
  if (!fb)
    return false;
  TransitionLocked(s, image,
                   image.isDepth ? plume::RenderTextureLayout::DEPTH_WRITE
                                 : plume::RenderTextureLayout::COLOR_WRITE);
  BindHelperFramebuffer(s, fb);
  if (image.isDepth) {
    s.command_list->clearDepthStencil(true, FormatHasStencil(image.format), surf.clearDepth, surf.clearStencil,
                                      nullptr, 0);
  } else {
    s.command_list->clearColor(0,
                               plume::RenderColor(surf.clearColor[0], surf.clearColor[1],
                                                  surf.clearColor[2], surf.clearColor[3]),
                               nullptr, 0);
  }
  image.needsClear = false;
  HelperPassDone(s);
  return true;
}

void LogTransfer(VideoState &s, const GuestSurface &surf, const char *what) {
  if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame())) {
    GpuTimingDiagMark(s, s.command_list, std::format("{} {:#x}", what, surf.va));
    EOT_INFO("[diag] {} {:#x} ({} t{} {}x{}, content {} in {}, agree {})", what, surf.va,
             surf.isDepth ? "depth" : "colour", surf.baseTile, surf.width, surf.height,
             static_cast<u32>(surf.content), surf.contentInSingle ? "single" : "host",
             surf.imagesAgree);
  }
}

void NoteHandoffRegret(const VideoState &s, GuestSurface &surf) {
  if (surf.handoffFrame != s.guest_frames || surf.handoffMirrorOrdinal >= 32)
    return;
  if (std::shared_ptr<GuestTexture> m = surf.handoffMirror.lock())
    m->handoffRegretMask |= 1u << surf.handoffMirrorOrdinal;
}

bool ResolveHostToSingle(VideoState &s, GuestSurface &surf) {
  auto *cmd = s.command_list;
  GpuTimingMark(s, cmd, kGpuCatResolveHw);
  LogTransfer(s, surf, "resolve host->single");
  NoteHandoffRegret(s, surf);
  surf.perfTransfers++;
  s.perf.surface_transfers++;
  surf.resolvedSinceDraw = true;
  const HostTextureTransition transitions[] = {
      {&surf.host, plume::RenderTextureLayout::RESOLVE_SOURCE},
      {&surf.single, plume::RenderTextureLayout::RESOLVE_DEST}};
  TransitionManyLocked(s, transitions, 2);
  {
    EOT_GPU_ZONE("resolve twin (hw)");
    cmd->resolveTexture(surf.single.texture.get(), surf.host.texture.get());
  }
  surf.single.needsClear = false;
  return true;
}

bool BroadcastSingleToHost(VideoState &s, GuestSurface &surf) {
  GpuTimingMark(s, s.command_list, kGpuCatBroadcast);
  LogTransfer(s, surf, "broadcast single->host");
  surf.perfTransfers++;
  s.perf.surface_transfers++;
  EOT_GPU_ZONE("broadcast twin");
  return HelperBlit(s, surf.single, surf.host,
                    GetBlitPipeline(s, surf.host.format, surf.host.sampleCount));
}

bool DeriveDepthSingle(VideoState &s, GuestSurface &surf) {
  GpuTimingMark(s, s.command_list, kGpuCatResolveDepth);
  LogTransfer(s, surf, surf.content == GuestSurface::Content::Cleared ? "clear depth twin"
                                                                        : "derive depth twin");
  surf.perfTransfers++;
  s.perf.surface_transfers++;
  if (surf.content == GuestSurface::Content::Cleared)
    return ClearImageToRemembered(s, surf, surf.single);
  if (s.stencil_ref_supported) {
    plume::RenderPipeline *pso =
        GetDeriveDepthStencilPipeline(s, surf.single.format, surf.host.sampleCount);
    const u32 stencil_index = pso ? BindStencilSRVLocked(s, surf.host) : kInvalidDescriptorIndex;
    if (pso && stencil_index != kInvalidDescriptorIndex) {
      EOT_GPU_ZONE("derive depth+stencil twin");
      return HelperBlit(s, surf.host, surf.single, pso, stencil_index);
    }
  }
  EOT_GPU_ZONE("derive depth twin");
  return HelperBlit(s, surf.host, surf.single,
                    GetResolveMsaaPipeline(s, surf.single.format, surf.host.sampleCount, true));
}

bool EnsureSurfaceSingle(VideoState &s, GuestSurface &surf) {
  if (surf.single.valid())
    return true;
  if (!CreateSurfaceImage(s, surf, surf.single, 1, surf.isDepth ? "surface-ds-1x" : "surface-rt-1x"))
    return false;
  EOT_DEBUG("[surfaces] {:#x}: single-sample twin {}x{} for the {}x image", surf.va,
            surf.single.width, surf.single.height, surf.host.sampleCount);
  if (surf.content == GuestSurface::Content::Cleared) {
    if (!ClearImageToRemembered(s, surf, surf.single))
      return false;
    if (surf.isDepth)
      surf.singleSerial = surf.writeSerial;
  }
  if (!surf.isDepth)
    surf.imagesAgree = surf.content != GuestSurface::Content::Drawn;
  return true;
}

}

HostTexture &SurfaceContentImage(GuestSurface &surf) {
  return (surf.contentInSingle || !surf.host.valid()) && surf.single.valid() ? surf.single : surf.host;
}

bool SurfaceMakeMultisampled(VideoState &s, GuestSurface &surf) {
  if (surf.host.valid() || surf.host.sampleCount <= 1)
    return surf.host.valid();
  const u32 samples = surf.host.sampleCount;
  const u64 t0 = PerfNow();
  if (!CreateSurfaceImage(s, surf, surf.host, samples, surf.isDepth ? "surface-ds" : "surface-rt")) {
    surf.host = DescribeSurfaceImage(surf, samples);
    return false;
  }
  if (!ClearImageToRemembered(s, surf, surf.host)) {
    DestroyHostTexture(s, surf.host);
    surf.host = DescribeSurfaceImage(surf, samples);
    return false;
  }
  if (surf.isDepth) {
    surf.singleDirty = surf.singleDirty || surf.content == GuestSurface::Content::Drawn;
    surf.singleSerial = surf.writeSerial;
  } else if (surf.content == GuestSurface::Content::Drawn) {
    surf.contentInSingle = true;
    surf.imagesAgree = false;
  } else if (surf.content != GuestSurface::Content::Borrowed) {
    surf.contentInSingle = false;
    surf.imagesAgree = true;
  }
  EOT_DEBUG("[surfaces] {:#x}: {} {}x{} {}x image made at its first multisampled pass ({} MB, {:.2f} ms)", surf.va,
            surf.isDepth ? "depth" : "colour", surf.host.width, surf.host.height, samples,
            HostTextureBytes(surf.host) >> 20, static_cast<f64>(PerfNow() - t0) * PerfMsPerTick());
  return true;
}

namespace {

void DropBorrow(GuestSurface &surf) {
  if (auto lender = surf.borrowed.lock()) {
    if (lender->borrower == &surf)
      lender->borrower = nullptr;
  }
  surf.borrowed.reset();
}

}

void SurfaceTransferToMirror(VideoState &s, GuestSurface &surf, HostTexture &src,
                             GuestTexture &target, const std::shared_ptr<GuestTexture> &target_ref) {
  const bool from_single = &src == &surf.single;
  const bool host_keeps = from_single && surf.host.sampleCount > 1 && surf.host.valid() &&
                          (!surf.contentInSingle || surf.imagesAgree);
  TextureReleaseBorrower(s, target);
  std::swap(target.host, src);
  target.bindingGeneration++;
  target.contentSerial++;
  src.needsClear = true;
  surf.contentInSingle = false;
  surf.imagesAgree = false;
  surf.handoffFrame = s.guest_frames;
  surf.handoffMirror = target_ref;
  DropBorrow(surf);
  if (host_keeps) {
    LogTransfer(s, surf, "hand twin to mirror");
  } else {
    surf.content = GuestSurface::Content::Borrowed;
    surf.borrowed = target_ref;
    surf.borrowedSerial = target.contentSerial;
    target.borrower = &surf;
    LogTransfer(s, surf, "hand content to mirror");
  }
  s.perf.resolve_transfers++;
}

void SurfaceRedirectBegin(VideoState &s, GuestSurface &surf) {
  if (surf.redirectPassFrame != s.guest_frames) {
    surf.redirectPassFrame = s.guest_frames;
    surf.redirectPasses = 0;
  }
  const u32 pass = surf.redirectPasses++;
  surf.redirectMirror.reset();
  if (pass >= GuestSurface::kRedirectPasses || !surf.isDepth ||
      surf.host.sampleCount != 1 || !surf.host.valid())
    return;
  const GuestSurface::RedirectPrediction &p = surf.redirectPredictions[pass];
  constexpr u32 kRedirectConfidence = 16;
  if (p.streak < kRedirectConfidence || p.recordedFrame + 1 != s.guest_frames)
    return;
  for (u32 j = 0; j < GuestSurface::kRedirectPasses; ++j) {
    const GuestSurface::RedirectPrediction &q = surf.redirectPredictions[j];
    if (j != pass && q.recordedFrame == s.guest_frames && q.texture == p.texture && q.x == p.x &&
        q.y == p.y)
      return;
  }
  std::shared_ptr<GuestTexture> m = p.mirror.lock();
  if (!m || m->aliasPending || !m->host.valid() || m->host.texture.get() != p.texture || !m->host.isDepth ||
      !m->host.renderable || m->host.format != surf.host.format || m->host.sampleCount != 1 ||
      m->host.mipLevels != 1 || m->host.arraySize != 1 || p.x < 0 || p.y < 0 ||
      p.x + surf.host.width > m->host.width || p.y + surf.host.height > m->host.height)
    return;
  surf.redirectMirror = std::move(m);
  surf.redirectX = p.x;
  surf.redirectY = p.y;
  LogTransfer(s, surf, "redirect pass to atlas");
}

bool SurfaceRedirectEnd(VideoState &s, GuestSurface &surf) {
  std::shared_ptr<GuestTexture> m = std::move(surf.redirectMirror);
  surf.redirectMirror.reset();
  if (surf.redirectPassFrame == s.guest_frames && surf.redirectPasses >= 1 &&
      surf.redirectPasses <= GuestSurface::kRedirectPasses)
    surf.redirectPredictions[surf.redirectPasses - 1].streak = 0;
  if (!m || !m->host.valid() || !surf.host.valid())
    return true;
  GpuTimingMark(s, s.command_list, kGpuCatResolveDepth);
  LogTransfer(s, surf, "redirect: copy region back");
  const float rect[4] = {
      static_cast<float>(surf.redirectX) / static_cast<float>(m->host.width),
      static_cast<float>(surf.redirectY) / static_cast<float>(m->host.height),
      static_cast<float>(surf.redirectX + surf.host.width) / static_cast<float>(m->host.width),
      static_cast<float>(surf.redirectY + surf.host.height) / static_cast<float>(m->host.height)};
  surf.perfTransfers++;
  s.perf.surface_transfers++;
  return HelperBlit(s, m->host, surf.host, GetDepthCopyPipeline(s, surf.host.format), 0, rect);
}

void SurfacesReleaseMirror(VideoState &s, const GuestTexture &t) {
  for (auto &[key, slot] : s.surfaces) {
    if (slot && slot->redirectMirror.get() == &t)
      slot->redirectMirror.reset();
  }
}

bool SurfaceTakeBack(VideoState &s, GuestSurface &surf) {
  if (surf.content != GuestSurface::Content::Borrowed)
    return true;
  std::shared_ptr<GuestTexture> lender = surf.borrowed.lock();
  const bool have = lender && lender->borrower == &surf &&
                    lender->contentSerial == surf.borrowedSerial && lender->host.valid();
  DropBorrow(surf);
  HostTexture &dst = surf.host.sampleCount > 1 ? surf.single : surf.host;
  if (!have || (surf.host.sampleCount > 1 && !EnsureSurfaceSingle(s, surf)) ||
      lender->host.width != dst.width || lender->host.height != dst.height ||
      lender->host.format != dst.format) {
    surf.content = GuestSurface::Content::Undefined;
    surf.serial++;
    surf.wholeClearSerial = surf.serial;
    surf.host.needsClear = true;
    if (surf.single.valid())
      surf.single.needsClear = true;
    LogTransfer(s, surf, "take back: content lost");
    u32 n;
    if (DiagShouldLog(0x7600 ^ surf.va, &n))
      EOT_DEBUG("[surfaces] {:#x}: borrowed content gone before the surface was drawn again", surf.va);
    return false;
  }
  GpuTimingMark(s, s.command_list, kGpuCatResolve);
  LogTransfer(s, surf, "take back from mirror");
  NoteHandoffRegret(s, surf);
  const HostTextureTransition transitions[] = {
      {&lender->host, plume::RenderTextureLayout::COPY_SOURCE},
      {&dst, plume::RenderTextureLayout::COPY_DEST}};
  TransitionManyLocked(s, transitions, 2);
  {
    EOT_GPU_ZONE("take back twin");
    s.command_list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(dst.texture.get(), 0, 0),
        plume::RenderTextureCopyLocation::Subresource(lender->host.texture.get(), 0, 0), 0, 0, 0,
        nullptr);
  }
  dst.needsClear = false;
  surf.content = GuestSurface::Content::Drawn;
  surf.contentInSingle = &dst == &surf.single;
  surf.imagesAgree = false;
  surf.perfTransfers++;
  s.perf.surface_transfers++;
  return true;
}

void TextureReleaseBorrower(VideoState &s, GuestTexture &t) {
  GuestSurface *surf = t.borrower;
  if (!surf)
    return;
  if (surf->content == GuestSurface::Content::Borrowed && surf->borrowed.lock().get() == &t)
    SurfaceTakeBack(s, *surf);
  t.borrower = nullptr;
}

void FlushAliasDependents(VideoState &s, GuestTexture &src) {
  std::vector<std::weak_ptr<GuestTexture>> dependents;
  dependents.swap(src.aliasDependents);
  for (const std::weak_ptr<GuestTexture> &weak : dependents) {
    std::shared_ptr<GuestTexture> d = weak.lock();
    if (d && d->aliasPending && d->aliasSource.lock().get() == &src)
      FlushAliasCopy(s, *d);
  }
}

bool FlushAliasCopy(VideoState &s, GuestTexture &t) {
  if (!t.aliasPending)
    return true;
  t.aliasPending = false;
  std::shared_ptr<GuestTexture> src = t.aliasSource.lock();
  t.aliasSource.reset();
  if (!src || !src->host.valid() || src->host.texture.get() != t.aliasSourceImage || !t.host.valid() ||
      src->host.format != t.host.format || src->host.isDepth != t.host.isDepth ||
      src->host.width < t.host.width || src->host.height < t.host.height || !s.command_list_open) {
    u32 n;
    if (DiagShouldLog(0x7700 ^ t.va, &n))
      EOT_DEBUG("[resolve] {:#x}: the alias source's image went before the alias was sampled", t.va);
    return false;
  }
  GpuTimingMark(s, s.command_list, t.host.isDepth ? kGpuCatResolveDepth : kGpuCatResolve);
  s.perf.alias_copies++;
  const bool same_size = src->host.width == t.host.width && src->host.height == t.host.height;
  if (t.host.isDepth && !same_size) {
    const float rect[4] = {0.0f, 0.0f, static_cast<float>(t.host.width) / static_cast<float>(src->host.width),
                           static_cast<float>(t.host.height) / static_cast<float>(src->host.height)};
    return HelperBlit(s, src->host, t.host, GetDepthCopyPipeline(s, t.host.format), 0, rect);
  }
  const HostTextureTransition transitions[] = {{&src->host, plume::RenderTextureLayout::COPY_SOURCE},
                                               {&t.host, plume::RenderTextureLayout::COPY_DEST}};
  TransitionManyLocked(s, transitions, 2);
  const plume::RenderBox box(0, 0, static_cast<i32>(t.host.width), static_cast<i32>(t.host.height));
  s.command_list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(t.host.texture.get(), 0, 0),
                                    plume::RenderTextureCopyLocation::Subresource(src->host.texture.get(), 0, 0),
                                    0, 0, 0, same_size ? nullptr : &box);
  t.host.needsClear = false;
  if (t.storeSwapRB != src->storeSwapRB) {
    t.storeSwapRB = src->storeSwapRB;
    t.bindingGeneration++;
  }
  return true;
}

HostTexture *SurfaceImageForDraw(VideoState &s, GuestSurface &surf, u32 samples,
                                 bool writes_color) {
  if (!SurfaceHasImage(surf))
    return nullptr;
  const bool want_single = samples == 1 && surf.host.sampleCount > 1;
  if (!want_single && !surf.host.valid() && !SurfaceMakeMultisampled(s, surf))
    return nullptr;
  if (!writes_color) {
    if (!want_single)
      return &surf.host;
    return EnsureSurfaceSingle(s, surf) ? &surf.single : nullptr;
  }
  if (surf.content == GuestSurface::Content::Borrowed)
    SurfaceTakeBack(s, surf);
  if (!want_single) {
    if (surf.contentInSingle && surf.single.valid() && !surf.imagesAgree &&
        surf.content == GuestSurface::Content::Drawn) {
      if (!BroadcastSingleToHost(s, surf))
        return nullptr;
      surf.imagesAgree = true;
    }
    surf.contentInSingle = false;
    return &surf.host;
  }
  if (!EnsureSurfaceSingle(s, surf))
    return nullptr;
  if (!surf.contentInSingle && !surf.imagesAgree && surf.content == GuestSurface::Content::Drawn &&
      surf.host.valid()) {
    if (!ResolveHostToSingle(s, surf))
      return nullptr;
    surf.imagesAgree = true;
  }
  surf.contentInSingle = true;
  return &surf.single;
}

HostTexture *SurfaceDepthSingle(VideoState &s, GuestSurface &surf, bool refresh) {
  if (!surf.isDepth || !SurfaceHasImage(surf))
    return nullptr;
  if (surf.host.sampleCount == 1)
    return &surf.host;
  if (!surf.host.valid())
    return &surf.single;
  if (!EnsureSurfaceSingle(s, surf))
    return nullptr;
  if (refresh && surf.singleSerial != surf.writeSerial) {
    if (surf.content != GuestSurface::Content::Undefined && !DeriveDepthSingle(s, surf))
      return nullptr;
    surf.singleSerial = surf.writeSerial;
  }
  return &surf.single;
}

HostTexture *SurfaceContentPeek(VideoState &s, GuestSurface &surf) {
  if (!surf.isDepth && surf.content == GuestSurface::Content::Borrowed) {
    std::shared_ptr<GuestTexture> lender = surf.borrowed.lock();
    if (lender && lender->borrower == &surf && lender->contentSerial == surf.borrowedSerial &&
        lender->host.valid())
      return &lender->host;
  }
  return SurfaceColorSingle(s, surf);
}

HostTexture *SurfaceColorSingle(VideoState &s, GuestSurface &surf) {
  if (surf.isDepth)
    return SurfaceDepthSingle(s, surf, true);
  if (!SurfaceHasImage(surf))
    return nullptr;
  if (surf.content == GuestSurface::Content::Borrowed)
    SurfaceTakeBack(s, surf);
  if (surf.host.sampleCount == 1)
    return &surf.host;
  if ((surf.contentInSingle || !surf.host.valid()) && surf.single.valid())
    return &surf.single;
  if (!EnsureSurfaceSingle(s, surf))
    return nullptr;
  if (!surf.imagesAgree && surf.content == GuestSurface::Content::Drawn) {
    if (!ResolveHostToSingle(s, surf))
      return nullptr;
    surf.imagesAgree = true;
  }
  return &surf.single;
}

void NoteSurfaceDrawn(GuestSurface &surf, const HostTexture &image, bool writes_depth_stencil,
                      bool writes_depth) {
  surf.drawn = true;
  const bool twin = &image == &surf.single;
  if (twin)
    surf.singleDraws++;
  else if (&image == &surf.host)
    surf.hostDraws++;
  if (surf.isDepth) {
    if (!writes_depth_stencil)
      return;
    if (writes_depth)
      surf.serial++;
    if (twin) {
      if (writes_depth)
        surf.singleDirty = true;
      return;
    }
    surf.content = GuestSurface::Content::Drawn;
    surf.writeSerial++;
    return;
  }
  surf.content = GuestSurface::Content::Drawn;
  surf.contentInSingle = twin;
  surf.imagesAgree = false;
  surf.serial++;
  if (!twin)
    surf.resolvedSinceDraw = false;
}

void NoteSurfaceClearedColor(GuestSurface &surf, const HostTexture &image, const float rgba[4],
                             bool whole, bool both) {
  surf.drawn = true;
  surf.serial++;
  if (whole) {
    surf.wholeClearSerial = surf.serial;
    surf.handoffFrame = ~0ull;
    surf.handoffMirror.reset();
    DropBorrow(surf);
    surf.content = GuestSurface::Content::Cleared;
    surf.resolvedSinceDraw = false;
    for (u32 i = 0; i < 4; ++i)
      surf.clearColor[i] = rgba[i];
    surf.contentInSingle = false;
    surf.imagesAgree = true;
    return;
  }
  DropBorrow(surf);
  surf.content = GuestSurface::Content::Drawn;
  if (!both)
    surf.contentInSingle = &image == &surf.single;
  surf.imagesAgree = both;
  if (&image == &surf.host && !both)
    surf.resolvedSinceDraw = false;
}

void NoteSurfaceClearedDepth(GuestSurface &surf, float depth, u8 stencil, bool whole, bool both) {
  surf.drawn = true;
  surf.writeSerial++;
  surf.serial++;
  if (both)
    surf.singleDirty = false;
  if (whole) {
    surf.content = GuestSurface::Content::Cleared;
    surf.clearDepth = depth;
    surf.clearStencil = stencil;
  } else {
    surf.content = GuestSurface::Content::Drawn;
  }
  if (both && surf.single.valid())
    surf.singleSerial = surf.writeSerial;
}

bool SurfacePropagateDepthSingle(VideoState &s, GuestSurface &surf) {
  if (!surf.isDepth || !surf.singleDirty || !surf.single.valid() || !surf.host.valid())
    return true;
  GpuTimingMark(s, s.command_list, kGpuCatBroadcast);
  LogTransfer(s, surf, "propagate depth single->host");
  surf.perfTransfers++;
  s.perf.surface_transfers++;
  EOT_GPU_ZONE("propagate depth twin");
  if (!HelperBlit(s, surf.single, surf.host,
                  GetDepthCopyPipeline(s, surf.host.format, surf.host.sampleCount)))
    return false;
  surf.singleDirty = false;
  surf.writeSerial++;
  surf.singleSerial = surf.writeSerial;
  return true;
}

void DestroySurfaceImages(VideoState &s, GuestSurface &surf) {
  DropBorrow(surf);
  DestroyHostTexture(s, surf.host);
  if (surf.single.valid())
    DestroyHostTexture(s, surf.single);
  surf.single = HostTexture{};
  surf.singleDirty = false;
  surf.contentInSingle = false;
  surf.imagesAgree = true;
  surf.resolvedSinceDraw = false;
  surf.content = GuestSurface::Content::Undefined;
}

}
