// gpu/resolve.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/wos_tiling.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/gpu_timing.h"
#include "gpu/render_thread.h"

#include "gpu/draw.h"
#include "gpu/format.h"
#include "gpu/settings.h"
#include "gpu/patches/aspect_ratio.h"
#include "gpu/surfaces.h"
#include "gpu/textures.h"
#include "gpu/trace.h"
#include "gpu/taa.h"

namespace eot::gpu {

// WoS port (fix67): a resolve of a D24FS8 depth surface keeps the guest's 20e4 precision
// (copy_depth_f24_ps). wos_recomp.toml wos_depth_float24 = false turns it off.
extern "C" bool wos_depth_float24();
static bool Float24DepthResolve(const GuestSurface &surf) {
  static const bool on = wos_depth_float24();
  if (!on || !surf.isDepth || (surf.depthFormat != 1 && (surf.formatWord & 0x3F) != 23))
    return false;
  static bool logged = false;
  if (!logged) {
    logged = true;
    EOT_INFO("[resolve] depth resolves of D24FS8 surfaces keep 20e4 precision (fix67)");
  }
  return true;
}

namespace xe = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;

namespace {

bool MirrorAtScale(const GuestTexture &t, float k) {
  return t.host.width == ScaleDimBy(t.width, k) && t.host.height == ScaleDimBy(t.height, k);
}

bool LocateAlias(const GuestTexture &d, const GuestTexture &t, i32 &tx, i32 &ty) {
  if (t.format != d.format || t.tiled != d.tiled)
    return false;
  const u32 pitch = ((d.fetch[0] >> 22) & 0x1FF) * 32;
  if (!pitch || pitch != ((t.fetch[0] >> 22) & 0x1FF) * 32)
    return false;
  const rex::graphics::FormatInfo *fi =
      rex::graphics::FormatInfo::Get(static_cast<u32>(d.format));
  if (!fi || fi->block_width != 1 || fi->block_height != 1)
    return false;
  const u32 bpb = fi->bytes_per_block();
  if (bpb < 4)
    return false;
  u32 bpb_log2 = 0;
  while ((1u << bpb_log2) < bpb)
    ++bpb_log2;
  const bool t_after = t.baseAddress >= d.baseAddress;
  const u32 delta = t_after ? t.baseAddress - d.baseAddress : d.baseAddress - t.baseAddress;
  i32 ox = 0, oy = 0;
  if (d.tiled) {
    const u32 tile_bytes = 32 * 32 * bpb;
    const u32 tiles_per_row = pitch / 32;
    if (!tiles_per_row || delta % tile_bytes)
      return false;
    const u32 tile = delta / tile_bytes;
    ox = static_cast<i32>((tile % tiles_per_row) * 32);
    oy = static_cast<i32>((tile / tiles_per_row) * 32);
    if (tu::GetTiledOffset2D(ox, oy, pitch, bpb_log2) != static_cast<i32>(delta))
      return false;
  } else {
    const u32 row_bytes = pitch * bpb;
    oy = static_cast<i32>(delta / row_bytes);
    ox = static_cast<i32>((delta % row_bytes) / bpb);
  }
  tx = t_after ? ox : -ox;
  ty = t_after ? oy : -oy;
  return true;
}

i32 Signed6(u32 v) {
  v &= 0x3F;
  return v & 0x20 ? static_cast<i32>(v) - 64 : static_cast<i32>(v);
}

u32 ResolveStoreSwizzle(u32 fetch3) {
  const u32 sw = (fetch3 >> 1) & 0xFFF;
  constexpr u32 kSwapRedBlue = 0x60A;
  return (sw & 7) == 2 ? (0x80000000u | kSwapRedBlue) : 0u;
}

void ClearSource(VideoState &s, GuestSurface &surf, const float *clear_rgba, float clear_z) {
  surf.redirectMirror.reset();
  float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (!surf.isDepth && clear_rgba) {
    for (u32 i = 0; i < 4; ++i)
      rgba[i] = clear_rgba[i];
  }
  auto clear_image = [&](HostTexture &image) {
    HostTexture *colors[4] = {surf.isDepth ? nullptr : &image, nullptr, nullptr, nullptr};
    plume::RenderFramebuffer *fb =
        GetFramebuffer(s, colors, surf.isDepth ? 0u : 1u, surf.isDepth ? &image : nullptr);
    if (!fb)
      return;
    TransitionLocked(s, image,
                     surf.isDepth ? plume::RenderTextureLayout::DEPTH_WRITE
                                  : plume::RenderTextureLayout::COLOR_WRITE);
    if (s.bound_framebuffer != fb)
      s.command_list->setFramebuffer(fb);
    s.bound_framebuffer = fb;
    s.bound_draw_targets_valid = false;
    if (surf.isDepth)
      s.command_list->clearDepthStencil(true, FormatHasStencil(image.format), clear_z, 0, nullptr, 0);
    else
      s.command_list->clearColor(0, plume::RenderColor(rgba[0], rgba[1], rgba[2], rgba[3]), nullptr, 0);
    image.needsClear = false;
  };
  if (surf.host.valid())
    clear_image(surf.host);
  if (SurfaceHasSingle(surf))
    clear_image(surf.single);
  if (surf.isDepth)
    NoteSurfaceClearedDepth(surf, clear_z, 0, true, SurfaceHasSingle(surf));
  else
    NoteSurfaceClearedColor(surf, surf.host, rgba, true, true);
  surf.perfClears++;
}

bool CaptureResolve(u32 device_va, u32 flags, u32 src_rect_va, u32 dest_texture_va,
                    u32 dest_point_va, u32 dest_level, u32 clear_color_va, float clear_z,
                    ResolvePacket &pk) {
  pk = ResolvePacket{};
  pk.device_va = device_va;
  pk.flags = flags;
  pk.destVa = dest_texture_va;
  pk.destLevel = dest_level;
  pk.clearZ = clear_z;
  const DeviceView dev = Device(device_va);
  const u32 source = flags & 7;
  const bool depth_source = source == 4;
  pk.srcVa = depth_source ? dev.U32(dev::kDepthSurface)
                          : dev.U32(dev::kRenderTarget0 + 4 * (source & 3));
  if (pk.srcVa && !ReadSurfaceHeaderWords(pk.srcVa, pk.srcWords))
    pk.srcVa = 0;
  if (pk.srcVa)
    wos_tiling::UseDrawWords(pk.srcVa, pk.srcWords);  // WoS port: the surface drawn into
  if (src_rect_va) {
    pk.hasRect = true;
    for (u32 i = 0; i < 4; ++i)
      pk.rect[i] = mem::load<i32>(src_rect_va + 4 * i);
  }
  if (dest_point_va) {
    pk.hasPoint = true;
    pk.point[0] = mem::load<i32>(dest_point_va);
    pk.point[1] = mem::load<i32>(dest_point_va + 4);
  }
  if (clear_color_va) {
    pk.hasColor = true;
    for (u32 i = 0; i < 4; ++i)
      pk.rgba[i] = mem::f32at(clear_color_va + 4 * i);
  }
  if ((flags & 0x200) && !depth_source) {
    pk.dsVa = dev.U32(dev::kDepthSurface);
    if (pk.dsVa && !ReadSurfaceHeaderWords(pk.dsVa, pk.dsWords))
      pk.dsVa = 0;
  }
  return true;
}

}

void ResolveGuest(u32 device_va, u32 flags, u32 src_rect_va, u32 dest_texture_va,
                  u32 dest_point_va, u32 dest_level, u32 clear_color_va, float clear_z) {
  auto &s = state();
  if (!s.ready)
    return;
  const bool clear_only = TakeMovieResolveSkip();
  if (RenderThreadActive()) {
    RenderEnqueue enqueue;
    RenderCommand &c = enqueue.cmd();
    c.type = RenderCommandType::Resolve;
    if (CaptureResolve(device_va, flags, src_rect_va, dest_texture_va, dest_point_va, dest_level,
                       clear_color_va, clear_z, c.resolve)) {
      c.resolve.clearDestOnly = clear_only;
      enqueue.commit(false);
    }
    return;
  }
  ResolvePacket pk;
  if (!CaptureResolve(device_va, flags, src_rect_va, dest_texture_va, dest_point_va, dest_level,
                      clear_color_va, clear_z, pk))
    return;
  pk.clearDestOnly = clear_only;
  std::lock_guard video_lock(s.mutex);
  ReplayResolveLocked(s, pk);
}

static void ClearResolveDestination(VideoState &s, const ResolvePacket &pk) {
  GuestSurface *surf = pk.srcVa ? GetGuestSurfaceWords(s, pk.srcVa, pk.srcWords) : nullptr;
  GuestTexture *dest = pk.destVa ? GetGuestTexture(s, pk.destVa, false) : nullptr;
  if (!surf || !dest || surf->isDepth || !EnsureResolveMirror(s, *dest, false, surf->scale) ||
      !dest->host.renderable)
    return;
  for (u32 m = 0; m < dest->host.mipLevels; ++m) {
    plume::RenderFramebuffer *fb = GetMipFramebuffer(s, *dest, m);
    if (!fb)
      continue;
    TransitionLocked(s, dest->host, plume::RenderTextureLayout::COLOR_WRITE);
    s.command_list->setFramebuffer(fb);
    s.command_list->clearColor(0, plume::RenderColor(0, 0, 0, 0), nullptr, 0);
  }
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  s.bound_draw_targets_valid = false;
  dest->host.needsClear = false;
  dest->uploaded = true;
  dest->uploadedUnlockSeq = ResourceUnlockSeq(dest->va);
  dest->contentSerial++;
  dest->lastUseFrame = s.guest_frames;
}

void ReplayResolveLocked(VideoState &s, const ResolvePacket &pk) {
  if (!s.ready)
    return;
  const u32 flags = pk.flags, dest_texture_va = pk.destVa, dest_level = pk.destLevel;
  const float clear_z = pk.clearZ;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  if (pk.clearDestOnly) {
    ClearResolveDestination(s, pk);
    return;
  }
  EOT_CPU_ZONE("ResolveGuest");
  PerfScope perf_scope(s.perf.resolve_ms);
  s.perf.resolves++;
  if (pk.refresh)
    s.perf.resolve_refreshes++;
  FlushPendingTransitions(s);
  GpuTimingMark(s, s.command_list, kGpuCatResolve);

  const u32 source = flags & 7;
  const bool depth_source = source == 4;
  const u32 src_va = pk.srcVa;
  GuestSurface *surf = src_va ? GetGuestSurfaceWords(s, src_va, pk.srcWords) : nullptr;
  if (!surf || !SurfaceHasImage(*surf)) {
    u32 n;
    if (DiagShouldLog(0x7001, &n))
      EOT_WARN("[resolve] source {} has no bound surface (flags {:#x})", source, flags);
    return;
  }
  surf->perfResolves++;

  if (!depth_source && surf->content == GuestSurface::Content::Borrowed)
    SurfaceTakeBack(s, *surf);
  HostTexture *src_host = depth_source ? &surf->host : &SurfaceContentImage(*surf);
  GuestSurface *alias_src = nullptr;
  {
    PerfScope msaa_scope(s.perf.msaa_scan_ms);
    alias_src = FindMultisampleAliasSource(s, *surf);
  }
  if (alias_src) {
    src_host = depth_source ? &alias_src->host : SurfaceContentPeek(s, *alias_src);
    if (!src_host)
      return;
    u32 n;
    if (DiagShouldLog(0x7400 ^ src_va, &n) && n == 0)
      EOT_DEBUG("[resolve] {:#x}: {}x{} {}x-msaa alias -> sampling {}x{} 1x surface {:#x}", src_va,
               surf->width, surf->height, surf->msaaSamples, alias_src->width, alias_src->height,
               alias_src->va);
  }
  if (src_host->sampleCount > 1) {
    GuestSurface &src_surf = alias_src ? *alias_src : *surf;
    if (HostTexture *single = SurfaceColorSingle(s, src_surf))
      src_host = single;
  }
  if (!src_host->valid()) {
    u32 n;
    if (DiagShouldLog(0x7B00 ^ src_va, &n))
      EOT_WARN("[resolve] {:#x}: no image to read (the {}x image not made, no twin)", src_va,
               surf->host.sampleCount);
    return;
  }

  GuestTexture *dest = nullptr;
  {
    EOT_CPU_ZONE("resolve destination mirror");
    PerfScope mirror_scope(s.perf.resolve_mirror_ms);
    dest = dest_texture_va ? GetGuestTexture(s, dest_texture_va, false) : nullptr;
    if (dest && !EnsureResolveMirror(s, *dest, depth_source, surf->scale,
                                     depth_source ? surf->host.format : plume::RenderFormat::UNKNOWN))
      dest = nullptr;
  }

  i32 x0 = 0, y0 = 0, x1 = static_cast<i32>(surf->width), y1 = static_cast<i32>(surf->height);
  if (pk.hasRect) {
    x0 = pk.rect[0];
    y0 = pk.rect[1];
    x1 = pk.rect[2];
    y1 = pk.rect[3];
  }
  i32 dx = 0, dy = 0;
  if (pk.hasPoint) {
    dx = pk.point[0];
    dy = pk.point[1];
  }
  x0 = std::max(0, x0);
  y0 = std::max(0, y0);
  x1 = std::min(static_cast<i32>(surf->width), x1);
  y1 = std::min(static_cast<i32>(surf->height), y1);
  const i32 rw = x1 - x0, rh = y1 - y0;

  const bool whole_src = x0 == 0 && y0 == 0 && rw == static_cast<i32>(surf->width) &&
                         rh == static_cast<i32>(surf->height);
  bool redirect_hit = false;
  if (depth_source && surf->redirectMirror) {
    const i32 hx = ScalePxBy(dx, surf->scale), hy = ScalePxBy(dy, surf->scale);
    if (dest == surf->redirectMirror.get() && whole_src && dest_level == 0 &&
        hx == surf->redirectX && hy == surf->redirectY) {
      redirect_hit = true;
      src_host = &surf->redirectMirror->host;
    } else if (!SurfaceRedirectEnd(s, *surf)) {
      return;
    }
  }
  const bool src_is_region = redirect_hit;
  const i32 src_origin_x = src_is_region ? surf->redirectX : 0;
  const i32 src_origin_y = src_is_region ? surf->redirectY : 0;

  if (dest && rw > 0 && rh > 0 && dest_level < dest->host.mipLevels) {
    const i32 copy_exp = Signed6(flags >> 26);
    const i32 tex_exp = Signed6(dest->fetch[3] >> 13);
    const i32 rt_exp = surf->colorExpBias;
    const i32 net_exp = rt_exp + copy_exp + tex_exp;
    const float scale = 1.0f;
    {
      u32 n;
      const bool diag_now =
          Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame());
      if ((DiagShouldLog(0x7100 ^ dest_texture_va ^ (dest_level << 8), &n) && n == 0) || diag_now) {
        EOT_DEBUG("[resolve] {} {:#x} -> tex {:#x} mip {} rect {},{}-{},{} at {},{} exp rt{} "
                  "copy{} tex{} => x{} ({} -> host fmt {}) fetch=[{:08x} {:08x} {:08x} {:08x}] "
                  "base {:#x} pitch {} {}x{} {}",
                  depth_source ? "depth" : "color", src_va, dest_texture_va, dest_level, x0, y0,
                  x1, y1, dx, dy, rt_exp, copy_exp, tex_exp, std::ldexp(1.0f, net_exp),
                  static_cast<u32>(dest->format), static_cast<u32>(dest->host.format),
                  dest->fetch[0], dest->fetch[1], dest->fetch[2], dest->fetch[3],
                  dest->baseAddress, ((dest->fetch[0] >> 22) & 0x1FF) * 32, dest->width,
                  dest->height, dest->tiled ? "tiled" : "linear");
      }
    }
    std::shared_ptr<GuestTexture> dest_ref;
    if (auto it = s.textures.find(dest_texture_va); it != s.textures.end())
      dest_ref = it->second;
    auto mark = [&](GuestTexture &target, u32 level) {
      if (!target.resolveOwned)
        NoteResolveDestination(s, target);
      target.resolveOwned = true;
      target.resolveProvisional = false;
      target.uploaded = true;
      target.uploadedUnlockSeq = ResourceUnlockSeq(target.va);
      target.resolvedMipMask |= 1u << level;
      target.lastUseFrame = s.guest_frames;
      if (target.lastResolvedFrame && target.lastSampledFrame < target.lastResolvedFrame) {
        s.perf.dead_resolves++;
        target.perfDeadResolves++;
      }
      target.lastResolvedFrame = s.guest_frames;
      target.perfResolves++;
    };
    plume::RenderViewport last_resolve_vp;
    plume::RenderRect last_resolve_sc;
    bool resolve_dynamic_valid = false;
    auto blit = [&](GuestTexture &target, u32 level, i32 vx, i32 vy, i32 vw, i32 vh, i32 sx0,
                    i32 sy0) -> bool {
      const i32 mip_w = static_cast<i32>(std::max(1u, target.width >> level));
      const i32 mip_h = static_cast<i32>(std::max(1u, target.height >> level));
      const bool whole = sx0 == 0 && sy0 == 0 && vx == 0 && vy == 0 &&
                         vw == static_cast<i32>(surf->width) &&
                         vh == static_cast<i32>(surf->height) && vw == mip_w && vh == mip_h;
      const bool same_format = src_host->format == target.host.format;
      // WoS port: a 7e3 target is clamped on its way out (see blit_ps.hlsl), so it always
      // takes the blit - never the image hand-off or a raw copy
      const bool clamp7e3 = !depth_source && (surf->colorFormat == 3 || surf->colorFormat == 12);
      const bool clampFixed = !depth_source && (surf->colorFormat == 4 || surf->colorFormat == 5);
      const bool reorder = !depth_source && ResolveStoreSwizzle(target.fetch[3]) != 0;
      const bool covers_image = whole && target.host.mipLevels == 1;
      const bool ms_src = src_host->sampleCount > 1;
      const GuestSurface &content_surf = alias_src ? *alias_src : *surf;
      const i32 rect_now[6] = {vx, vy, vw, vh, sx0, sy0};
      if (s.guest_frames - target.handoffRegretResetFrame >= 128) {
        target.handoffRegretResetFrame = s.guest_frames;
        target.handoffRegretMask = 0;
      }
      if (target.resolveOrdinalFrame != s.guest_frames) {
        target.resolveOrdinalFrame = s.guest_frames;
        target.resolveOrdinal = 0;
      }
      const u32 ordinal = std::min(target.resolveOrdinal, 31u);
      const bool regretted = pk.refresh || ((target.handoffRegretMask >> ordinal) & 1u);
      if (!pk.refresh)
        target.resolveOrdinal++;
      if (target.resolvedSurfaceUid == content_surf.uid &&
          target.resolvedSurfaceSerial == content_surf.serial &&
          target.resolvedOwnSerial == target.contentSerial && target.resolvedLevel == level &&
          std::memcmp(target.resolvedRect, rect_now, sizeof(rect_now)) == 0 &&
          !target.host.needsClear && !reorder == !target.storeSwapRB) {
        s.perf.resolve_noops++;
        mark(target, level);
        return true;
      }
      if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()))
        EOT_INFO("[diag] resolve {:#x} -> {:#x} not a no-op: uid {}/{} serial {}/{} own {}/{} level {}/{} "
                 "rect {},{},{},{},{},{}/{},{},{},{},{},{} needsClear {} reorder {} swap {}",
                 src_va, dest_texture_va, target.resolvedSurfaceUid, content_surf.uid,
                 target.resolvedSurfaceSerial, content_surf.serial, target.resolvedOwnSerial,
                 target.contentSerial, target.resolvedLevel, level, target.resolvedRect[0],
                 target.resolvedRect[1], target.resolvedRect[2], target.resolvedRect[3],
                 target.resolvedRect[4], target.resolvedRect[5], vx, vy, vw, vh, sx0, sy0,
                 target.host.needsClear, reorder, target.storeSwapRB);
      TextureReleaseBorrower(s, target);
      if (target.aliasPending) {
        if (covers_image) {
          target.aliasPending = false;
          target.aliasSource.reset();
        } else {
          FlushAliasCopy(s, target);
          resolve_dynamic_valid = false;
        }
      }
      target.contentSerial++;
      target.resolvedSurfaceUid = content_surf.uid;
      target.resolvedSurfaceSerial = content_surf.serial;
      target.velocity = content_surf.isDepth ? velocity::HandleFor(content_surf, s.guest_frames)
                                             : VelocityHandle{};
      target.resolvedOwnSerial = target.contentSerial;
      target.resolvedLevel = level;
      std::memcpy(target.resolvedRect, rect_now, sizeof(rect_now));
      const float k = surf->scale;
      const i32 mip_w_host = static_cast<i32>(std::max(1u, target.host.width >> level));
      const i32 mip_h_host = static_cast<i32>(std::max(1u, target.host.height >> level));
      const bool at_k = MirrorAtScale(target, k);
      const float kx = at_k ? k : static_cast<float>(mip_w_host) / static_cast<float>(mip_w);
      const float ky = at_k ? k : static_cast<float>(mip_h_host) / static_cast<float>(mip_h);
      const i32 hx0 = ScalePxBy(vx, kx), hy0 = ScalePxBy(vy, ky), hx1 = ScalePxBy(vx + vw, kx),
                hy1 = ScalePxBy(vy + vh, ky);
      if (hx1 <= hx0 || hy1 <= hy0)
        return true;
      const i32 sx0h = ScalePxBy(sx0, k) + src_origin_x, sy0h = ScalePxBy(sy0, k) + src_origin_y;
      const i32 sw = at_k ? hx1 - hx0 : ScalePxBy(sx0 + vw, k) - ScalePxBy(sx0, k);
      const i32 sh = at_k ? hy1 - hy0 : ScalePxBy(sy0 + vh, k) - ScalePxBy(sy0, k);
      if (!at_k) {
        u32 n;
        if (DiagShouldLog(0x7A00 ^ target.va ^ static_cast<u32>(k * 1000.0f), &n) && n == 0)
          EOT_DEBUG("[resolve] {:#x} {}x{} at x{:.3f} (tile {}) into {:#x} {}x{} fmt {}, a mirror made at "
                    "x{:.3f} ({}x{} host): drawn scaled, {}x{} -> {}x{} host",
                    src_va, surf->width, surf->height, k, surf->baseTile, target.va, target.width,
                    target.height, static_cast<u32>(target.format), kx, target.host.width,
                    target.host.height, sw, sh, hx1 - hx0, hy1 - hy0);
      }
      const bool copy_fits = at_k && sx0h + (hx1 - hx0) <= static_cast<i32>(src_host->width) &&
                             sy0h + (hy1 - hy0) <= static_cast<i32>(src_host->height) &&
                             hx1 <= mip_w_host && hy1 <= mip_h_host;
      const bool whole_host = whole && src_host->width == static_cast<u32>(mip_w_host) &&
                              src_host->height == static_cast<u32>(mip_h_host);
      const bool own_image = src_host == &surf->single || src_host == &surf->host;
      if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()) &&
          !depth_source && own_image)
        EOT_INFO("[diag] resolve {:#x} -> {:#x} hand-off gate: ordinal {} regretted {} (mask {:#x}) dest {} "
                 "ms {} alias {} fmt {} scale {} level {} whole_host {} covers {} array {} depth {} "
                 "samples {} flags {}/{} dim {}/{} committed {}/{}",
                 src_va, dest_texture_va, ordinal, regretted, target.handoffRegretMask, &target == dest && dest_ref,
                 ms_src, alias_src != nullptr, same_format, scale, level, whole_host, covers_image,
                 target.host.arraySize,
                 target.host.depth, target.host.sampleCount, static_cast<u32>(target.host.desc.flags),
                 static_cast<u32>(src_host->desc.flags), static_cast<u32>(target.host.desc.dimension),
                 static_cast<u32>(src_host->desc.dimension), target.host.desc.committed,
                 src_host->desc.committed);
      if (!clamp7e3 && !clampFixed && !regretted && &target == dest && dest_ref && !depth_source &&
          !ms_src && !alias_src && own_image && same_format && scale == 1.0f && level == 0 &&
          at_k && whole_host && covers_image && target.host.arraySize == 1 && target.host.depth == 1 &&
          target.host.sampleCount == 1 &&
          target.host.desc.flags == src_host->desc.flags &&
          target.host.desc.dimension == src_host->desc.dimension &&
          target.host.desc.committed == src_host->desc.committed &&
          target.host.transientPool == src_host->transientPool) {
        if (target.storeSwapRB != reorder) {
          target.storeSwapRB = reorder;
          target.bindingGeneration++;
        }
        target.host.needsClear = false;
        surf->handoffMirrorOrdinal = ordinal;
        SurfaceTransferToMirror(s, *surf, *src_host, target, dest_ref);
        src_host = &target.host;
        mark(target, level);
        return true;
      }
      if (!clamp7e3 && !clampFixed && !ms_src && !alias_src && same_format && scale == 1.0f && copy_fits &&
          (!depth_source || whole_host) && (covers_image || !target.host.needsClear) &&
          (!reorder || covers_image)) {
        if (covers_image)
          target.host.needsClear = false;
        if (target.storeSwapRB != reorder) {
          target.storeSwapRB = reorder;
          target.bindingGeneration++;
        }
        auto *cmd = s.command_list;
        const HostTextureTransition transitions[] = {
            {src_host, plume::RenderTextureLayout::COPY_SOURCE},
            {&target.host, plume::RenderTextureLayout::COPY_DEST}};
        TransitionManyLocked(s, transitions, 2);
        const plume::RenderBox box(sx0h, sy0h, sx0h + (hx1 - hx0), sy0h + (hy1 - hy0));
        if (GpuTimingDiagActive(s))
          GpuTimingDiagMark(s, cmd,
                            std::format("resolve copy {:#x} {}x{} {}", dest_texture_va, hx1 - hx0,
                                        hy1 - hy0, depth_source ? "depth" : "colour"));
        cmd->copyTextureRegion(
            plume::RenderTextureCopyLocation::Subresource(target.host.texture.get(), level, 0),
            plume::RenderTextureCopyLocation::Subresource(src_host->texture.get(), 0, 0),
            static_cast<u32>(hx0), static_cast<u32>(hy0), 0, whole_host ? nullptr : &box);
        s.perf.resolve_copies++;
        mark(target, level);
        return true;
      }
      {
        u32 n;
        const u32 key = 0x7500 ^ (static_cast<u32>(src_host->format) << 8) ^
                        static_cast<u32>(target.host.format) ^ (reorder ? 0x40000 : 0) ^
                        (alias_src ? 0x80000 : 0) ^ (depth_source ? 0x100000 : 0) ^
                        (ms_src ? 0x200000 : 0);
        if (DiagShouldLog(key, &n) && n == 0)
          EOT_DEBUG("[resolve] blit kept: host fmt {} -> {}{}{}{}{}", static_cast<u32>(src_host->format),
                   static_cast<u32>(target.host.format), reorder ? " reorder" : "",
                   alias_src ? " msaa-alias" : "", depth_source && !whole ? " depth-subrect" : "",
                   ms_src ? " multisampled-source" : "");
      }
      plume::RenderFramebuffer *fb = nullptr;
      {
        EOT_CPU_ZONE("resolve framebuffer");
        PerfScope fb_scope(s.perf.resolve_fb_ms);
        fb = GetMipFramebuffer(s, target, level);
      }
      if (!fb)
        return false;
      auto *cmd = s.command_list;
      PerfScope bind_scope(s.perf.resolve_bind_ms);
      const HostTextureTransition transitions[] = {
          {src_host, plume::RenderTextureLayout::SHADER_READ},
          {&target.host, depth_source ? plume::RenderTextureLayout::DEPTH_WRITE
                                      : plume::RenderTextureLayout::COLOR_WRITE}};
      TransitionManyLocked(s, transitions, 2);
      if (target.host.needsClear) {
        for (u32 m = 0; m < target.host.mipLevels; ++m) {
          plume::RenderFramebuffer *mfb = m == level ? fb : GetMipFramebuffer(s, target, m);
          if (!mfb)
            continue;
          if (s.bound_framebuffer != mfb) {
            cmd->setFramebuffer(mfb);
            s.bound_framebuffer = mfb;
          }
          s.bound_draw_targets_valid = false;
          if (target.host.isDepth)
            cmd->clearDepthStencil(true, FormatHasStencil(target.host.format), 0.0f, 0, nullptr, 0);
          else
            cmd->clearColor(0, plume::RenderColor(0, 0, 0, 0), nullptr, 0);
        }
        target.host.needsClear = false;
      }
      GpuTimingMark(s, cmd, depth_source ? kGpuCatResolveDepth : kGpuCatResolve);
      if (GpuTimingDiagActive(s))
        GpuTimingDiagMark(s, cmd,
                          std::format("resolve blit {:#x} {}x{} {}{}", dest_texture_va, hx1 - hx0,
                                      hy1 - hy0, depth_source ? "depth" : "colour",
                                      ms_src ? " ms" : ""));
      if (target.storeSwapRB) {
        target.storeSwapRB = false;
        target.bindingGeneration++;
      }
      if (s.bound_framebuffer != fb) {
        cmd->setFramebuffer(fb);
        s.bound_framebuffer = fb;
      }
      s.bound_draw_targets_valid = false;
      const plume::RenderFormat color_fmt = target.host.format;
      plume::RenderPipeline *pso =
          ms_src ? GetResolveMsaaPipeline(s, depth_source ? target.host.format : color_fmt,
                                          src_host->sampleCount, depth_source)
          : depth_source ? GetDepthCopyPipeline(s, target.host.format, 1,
                                                 Float24DepthResolve(*surf))
                         : GetBlitPipeline(s, color_fmt);
      if (!pso)
        return false;
      if (s.bound_pipeline != pso) {
        cmd->setPipeline(pso);
        s.bound_pipeline = pso;
      }
      plume::RenderViewport vp(static_cast<float>(hx0), static_cast<float>(hy0),
                               static_cast<float>(hx1 - hx0), static_cast<float>(hy1 - hy0), 0.0f,
                               1.0f);
      plume::RenderRect sc(hx0, hy0, hx1, hy1);
      if (!resolve_dynamic_valid ||
          std::memcmp(&last_resolve_vp, &vp, sizeof(last_resolve_vp)) != 0) {
        cmd->setViewports(&vp, 1);
        last_resolve_vp = vp;
      }
      if (!resolve_dynamic_valid ||
          std::memcmp(&last_resolve_sc, &sc, sizeof(last_resolve_sc)) != 0) {
        cmd->setScissors(&sc, 1);
        last_resolve_sc = sc;
      }
      resolve_dynamic_valid = true;
      CopyPushConstants pc;
      pc.resourceDescriptorIndex = BindTextureSRVLocked(s, *src_host);
      pc.resourceDescriptorIndex2 = depth_source ? 0u : ResolveStoreSwizzle(target.fetch[3]);
      pc.param0 = scale;
      pc.param1 = 0.0f;
      pc.extra[2] = clamp7e3 ? 1.0f : (clampFixed ? 2.0f : 0.0f);
      const GuestSurface &src_surf = alias_src ? *alias_src : *surf;
      if (src_is_region) {
        pc.rect[0] = static_cast<float>(sx0h) / static_cast<float>(src_host->width);
        pc.rect[1] = static_cast<float>(sy0h) / static_cast<float>(src_host->height);
        pc.rect[2] = static_cast<float>(sx0h + sw) / static_cast<float>(src_host->width);
        pc.rect[3] = static_cast<float>(sy0h + sh) / static_cast<float>(src_host->height);
      } else {
        const float rx = static_cast<float>(src_surf.width) / static_cast<float>(surf->width);
        const float ry = static_cast<float>(src_surf.height) / static_cast<float>(surf->height);
        pc.rect[0] = static_cast<float>(sx0) * rx / static_cast<float>(src_surf.allocWidth);
        pc.rect[1] = static_cast<float>(sy0) * ry / static_cast<float>(src_surf.allocHeight);
        pc.rect[2] = static_cast<float>(sx0 + vw) * rx / static_cast<float>(src_surf.allocWidth);
        pc.rect[3] = static_cast<float>(sy0 + vh) * ry / static_cast<float>(src_surf.allocHeight);
      }
      bind_scope.stop();
      cmd->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc,
                                    kCopyPushConstantByteOffset, sizeof(pc));
      {
        EOT_GPU_ZONE("resolve blit");
        cmd->drawInstanced(3, 1, 0, 0);
      }
      mark(target, level);
      return true;
    };

    const u32 mip_w = std::max(1u, dest->width >> dest_level);
    const u32 mip_h = std::max(1u, dest->height >> dest_level);
    const i32 vx = std::clamp(dx, 0, static_cast<i32>(mip_w));
    const i32 vy = std::clamp(dy, 0, static_cast<i32>(mip_h));
    const i32 vw = std::min(rw, static_cast<i32>(mip_w) - vx);
    const i32 vh = std::min(rh, static_cast<i32>(mip_h) - vy);
    if (whole_src && dest_level == 0 && !depth_source && vw > 0 && vh > 0 &&
        (vx != 0 || vy != 0 || vw < static_cast<i32>(mip_w) || vh < static_cast<i32>(mip_h))) {
      u32 n;
      if (DiagShouldLog(0x7900 ^ dest_texture_va ^ (static_cast<u32>(vw) << 12) ^ static_cast<u32>(vh), &n) && n == 0)
        EOT_DEBUG("[resolve] {:#x} {}x{} (alloc {}x{}, x{:.3f}, tile {}) covers part of texture {:#x} {}x{} fmt {} "
                  "(host {}x{}): {}x{} at {},{}, {}x{} host",
                  src_va, surf->width, surf->height, surf->allocWidth, surf->allocHeight, surf->scale, surf->baseTile,
                  dest_texture_va, dest->width, dest->height, static_cast<u32>(dest->format), dest->host.width,
                  dest->host.height, vw, vh, vx, vy, ScalePxBy(vw, surf->scale), ScalePxBy(vh, surf->scale));
    }
    if (vw > 0 && vh > 0 && redirect_hit) {
      dest->contentSerial++;
      dest->resolvedSurfaceUid = surf->uid;
      dest->resolvedSurfaceSerial = surf->serial;
      dest->velocity = surf->isDepth ? velocity::HandleFor(*surf, s.guest_frames) : VelocityHandle{};
      dest->resolvedOwnSerial = dest->contentSerial;
      dest->resolvedLevel = dest_level;
      const i32 rect_now[6] = {vx, vy, vw, vh, x0, y0};
      std::memcpy(dest->resolvedRect, rect_now, sizeof(rect_now));
      dest->host.needsClear = false;
      s.perf.resolve_noops++;
      mark(*dest, dest_level);
    } else if (vw > 0 && vh > 0) {
      blit(*dest, dest_level, vx, vy, vw, vh, x0, y0);
    }
    // WoS port (fix101): what was last written at this base, and in which format
    if (vw > 0 && vh > 0) {
      dest->resolveWriteSeq = ++s.resolve_write_seq;
      s.resolve_writes[dest->baseAddress] = {dest->resolveWriteSeq, static_cast<u32>(dest->format)};
    }
    if (!depth_source && !pk.refresh)
      dest->lastResolve = std::make_shared<ResolvePacket>(pk);
    if (depth_source && whole_src && dest_level == 0 && vw == rw && vh == rh &&
        surf->host.sampleCount == 1 && dest_ref && dest->host.isDepth && dest->host.renderable &&
        dest->host.sampleCount == 1 && dest->host.mipLevels == 1 && dest->host.arraySize == 1 &&
        dest->host.format == surf->host.format && surf->redirectPassFrame == s.guest_frames &&
        surf->redirectPasses >= 1 && surf->redirectPasses <= GuestSurface::kRedirectPasses &&
        MirrorAtScale(*dest, surf->scale) &&
        (dest->host.width > surf->host.width || dest->host.height > surf->host.height)) {
      const i32 hx = ScalePxBy(vx, surf->scale), hy = ScalePxBy(vy, surf->scale);
      if (hx >= 0 && hy >= 0 && hx + surf->host.width <= dest->host.width &&
          hy + surf->host.height <= dest->host.height) {
        GuestSurface::RedirectPrediction &p = surf->redirectPredictions[surf->redirectPasses - 1];
        const bool same = p.recordedFrame + 1 == s.guest_frames && p.mirror.lock() == dest_ref &&
                          p.texture == dest->host.texture.get() && p.x == hx && p.y == hy;
        p.streak = same ? p.streak + 1 : 0;
        p.recordedFrame = s.guest_frames;
        p.mirror = dest_ref;
        p.texture = dest->host.texture.get();
        p.x = hx;
        p.y = hy;
      }
    }

    if (dest_level == 0 && vw > 0 && vh > 0) {
      PerfScope alias_scope(s.perf.alias_scan_ms);
      const u64 gen = (s.texture_generation.load(std::memory_order_relaxed) << 20) ^
                      s.mirror_generation;
      if (s.resolve_alias_candidates_generation != gen) {
        s.resolve_alias_candidates_generation = gen;
        s.resolve_alias_candidates.clear();
        for (auto &[va, tex] : s.textures) {
          if (tex && tex->host.texture && tex->host.renderable)
            s.resolve_alias_candidates.push_back(tex.get());
        }
      }
      s.perf.alias_scanned += static_cast<u32>(s.resolve_alias_candidates.size());
      const u64 visit_token = ++s.resolve_alias_token;
      for (GuestTexture *t : s.resolve_alias_candidates) {
        const u32 alias_va = t->va;
        if (t == dest || !t->host.texture || !t->host.renderable ||
            t->host.isDepth != depth_source)
          continue;
        if (t->uploaded && !t->resolveOwned)
          continue;
        if (t->aliasVisitToken == visit_token)
          continue;
        t->aliasVisitToken = visit_token;
        i32 tx, ty;
        if (!LocateAlias(*dest, *t, tx, ty))
          continue;
        const i32 ax0 = vx - tx, ay0 = vy - ty;
        const i32 cx0 = std::max(ax0, 0), cy0 = std::max(ay0, 0);
        const i32 cx1 = std::min(ax0 + vw, static_cast<i32>(t->width));
        const i32 cy1 = std::min(ay0 + vh, static_cast<i32>(t->height));
        if (cx1 <= cx0 || cy1 <= cy0)
          continue;
        if (t->resolveProvisional &&
            (!EnsureResolveMirror(s, *t, depth_source, surf->scale,
                                  depth_source ? surf->host.format : plume::RenderFormat::UNKNOWN) ||
             !t->host.texture || !t->host.renderable || t->host.isDepth != depth_source))
          continue;
        {
          u32 n;
          if (DiagShouldLog(0x7300 ^ alias_va ^ dest_texture_va, &n) && n == 0)
            EOT_DEBUG("[resolve] {:#x} also lands in {:#x} ({}x{} base {:#x}) at {},{} ({}x{})",
                     dest_texture_va, alias_va, t->width, t->height, t->baseAddress, cx0, cy0,
                     cx1 - cx0, cy1 - cy0);
        }
        const bool deferrable =
            dest_ref.get() == dest && tx == 0 && ty == 0 && vx == 0 && vy == 0 &&
            vw == static_cast<i32>(dest->width) && vh == static_cast<i32>(dest->height) &&
            cx1 == static_cast<i32>(t->width) && cy1 == static_cast<i32>(t->height) &&
            MirrorAtScale(*dest, surf->scale) && MirrorAtScale(*t, surf->scale) &&
            dest->host.valid() && t->host.mipLevels == 1 && t->host.arraySize == 1 &&
            t->host.depth == 1 && t->host.sampleCount == 1 && dest->host.sampleCount == 1 &&
            t->host.format == dest->host.format && t->host.width <= dest->host.width &&
            t->host.height <= dest->host.height &&
            (depth_source || ResolveStoreSwizzle(t->fetch[3]) == ResolveStoreSwizzle(dest->fetch[3]));
        std::shared_ptr<GuestTexture> t_ref;
        if (deferrable)
          if (auto it = s.textures.find(alias_va); it != s.textures.end() && it->second.get() == t)
            t_ref = it->second;
        if (t_ref) {
          TextureReleaseBorrower(s, *t);
          t->contentSerial++;
          t->aliasPending = true;
          t->aliasSource = dest_ref;
          t->aliasSourceImage = dest->host.texture.get();
          t->velocity = dest->velocity;
          auto &dependents = dest->aliasDependents;
          bool noted = false;
          for (const auto &weak : dependents)
            noted |= weak.lock() == t_ref;
          if (!noted) {
            std::erase_if(dependents, [](const std::weak_ptr<GuestTexture> &w) { return w.expired(); });
            dependents.push_back(t_ref);
          }
          s.perf.alias_deferred++;
          mark(*t, 0);
          continue;
        }
        blit(*t, 0, cx0, cy0, cx1 - cx0, cy1 - cy0, x0 + (cx0 - ax0), y0 + (cy0 - ay0));
      }
    }
    s.bound_pipeline = nullptr;
    s.bound_framebuffer = nullptr;
  } else if (!dest) {
    u32 n;
    if (DiagShouldLog(0x7002 ^ dest_texture_va, &n))
      EOT_WARN("[resolve] destination {:#x} has no usable host mirror", dest_texture_va);
  }

  // WoS port: eot_diag_dump alone dumps guest frame 3000's resolves (no detail log needed)
  if (Settings::DiagDump() &&
      ((Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame())) ||
       s.guest_frames + 1 == static_cast<u64>(Settings::DumpFrame()))) {
    static u32 k = 0;
    static u32 stats = 0;
    // WoS port: on the console, beside the log; only the first diagnostic frame's
    // scene-sized resolves (a 1080p dump is 6 MB) - and (fix34) each re-armed dump frame's
    static u64 first_frame = 0;
    static i32 armed_for = 0;
    if (!first_frame || (armed_for != Settings::DumpFrame() &&
                         s.guest_frames + 1 == static_cast<u64>(Settings::DumpFrame()))) {
      first_frame = s.guest_frames + 1;
      armed_for = Settings::DumpFrame();
      k = 0;
      stats = 0;
      ::mkdir("/app0/userdata/wos_recomp/dump", 0777);
    }
    const std::string path =
        std::format("/app0/userdata/wos_recomp/dump/f{}_r{:02}_{}_{:x}_to_{:x}.ppm",
                    s.guest_frames + 1, k++, depth_source ? "depth" : "color", src_va,
                    dest_texture_va);
    if (surf->host.sampleCount == 1 && first_frame == s.guest_frames + 1 && surf->width >= 512)
      DumpHostTextureLocked(s, surf->host, path.c_str(), depth_source ? 1.0f : 1.0f);
    // WoS port: the exact values each colour resolve left in its destination, any size
    if (!depth_source && dest && dest->host.texture && first_frame == s.guest_frames + 1 && s.command_list_open) {
      if (stats++ < 96) {
        const std::string label =
            std::format("resolve {:x} -> tex {:x} base {:x} {}x{} fmt {} rect {},{}-{},{} surf {}x{} scale {}",
                        src_va, dest_texture_va, dest->baseAddress, dest->width, dest->height,
                        static_cast<u32>(dest->format), x0, y0, x1, y1, surf->width, surf->height, surf->scale);
        LogHostTextureStatsLocked(s, dest->host, label.c_str());
      }
    }
    if (!s.command_list_open)
      return;
  }
  if (flags & 0x100)
    ClearSource(s, *surf, pk.hasColor ? pk.rgba : nullptr, clear_z);
  if ((flags & 0x200) && depth_source)
    ClearSource(s, *surf, nullptr, clear_z);
  else if (flags & 0x200) {
    if (GuestSurface *ds = pk.dsVa ? GetGuestSurfaceWords(s, pk.dsVa, pk.dsWords) : nullptr)
      ClearSource(s, *ds, nullptr, clear_z);
  }
  s.bound_framebuffer = nullptr;
  DrainHostDebugMessages(s, "resolve");
}

}
