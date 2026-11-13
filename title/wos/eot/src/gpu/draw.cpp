#include <pthread.h>
#include <sys/stat.h>
// gpu/draw.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/wos_tiling.h"
#include "gpu/draw.h"

#include <unordered_map>

#include <xxhash.h>

#include "core/profiling.h"

#include "gpu/settings.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/memory/utils.h>

#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#else
#include <plume_vulkan.h>
#endif

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/debug/freecam.h"
#include "gpu/render_thread.h"
#include "gpu/backend.h"

// WoS port (fix34): the GPU register mirror (wos/app/wos_ps5_native_renderer.cpp) writes the
// stream's texture fetch constants and render state over a draw's captured device window
extern "C" void wos_regs_overlay(uint32_t device, uint8_t *image, uint32_t image_bytes, uint32_t sync);
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/gpu_timing.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/textures.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/surfaces.h"
#include "gpu/taa.h"
#include "gpu/trace.h"
#include "gpu/vertex_layout.h"

extern "C" bool wos_depth_shells();  // wos/app/wos_ps5_native_renderer.cpp (fix60)
extern "C" bool wos_debug_no_shadow_maps();  // (fix68)
extern "C" int wos_debug_null_slots();  // (fix69)
extern "C" bool wos_facade_reflections();  // (fix74)
extern "C" int wos_facade_max_distance();  // (fix77)
extern "C" int wos_facade_occlusion();     // (fix116)
extern "C" int wos_strip_fast_path();      // (fix117)
extern "C" bool wos_diag_extra();  // (fix78) wos_debug_draw_stats since fix80
extern "C" bool wos_fast_capture_on();  // (fix80) performance master switch
extern "C" bool wos_debug_skip_shadow_casters();  // (fix84) diagnostic
extern "C" bool wos_debug_no_cull();              // (fix86) diagnostic
extern "C" bool wos_debug_depth_always();         // (fix86) diagnostic
extern "C" bool wos_debug_no_stencil();           // (fix86) diagnostic
extern "C" bool wos_debug_no_zshift();            // (fix87) diagnostic
extern "C" bool wos_debug_depth_clamp();          // (fix87) diagnostic
extern "C" bool wos_dump_small();
extern "C" int wos_dump_small_from();
extern "C" const char *wos_skip_shaders();
extern "C" bool wos_instancing();
extern "C" int wos_dump_small_to();                 // (fix89) diagnostic
extern "C" int wos_dump_every();  // (fix85) mid-frame dump cadence
extern "C" int wos_dump_max();

namespace eot::gpu {

namespace {

namespace xe = rex::graphics::xenos;

constexpr u32 kPrimTriangleStrip = 6;
constexpr u32 kPrimTriangleFan = 5;
constexpr u32 kPrimRectList = 8;
constexpr u32 kPrimQuadList = 13;
constexpr u32 kPrimLineStrip = 3;

struct Targets {
  GuestSurface *color[4] = {};
  u32 colorCount = 0;
  GuestSurface *depth = nullptr;
  HostTexture *colorImage[4] = {};
  HostTexture *depthImage = nullptr;
  u32 samples = 1;
  bool writesDepthStencil = false;
  bool writesDepth = false;
  bool writesColor = true;
  bool additive = false;
  u32 width = 0;
  u32 height = 0;
  float scale = 1.0f;
  i32 offsetX = 0;
  i32 offsetY = 0;
  velocity::Target *velocity = nullptr;
};

struct ViewportInfo {
  plume::RenderViewport vp;
  plume::RenderRect scissor;
  float posScale[4] = {1, 1, 1, 1};
  float posOffset[4] = {0, 0, 0, 0};
};

void Dropped(const char *why, u64 site) {
  trace::Bump(trace::Counter::DrawDropped);
  u32 n;
  if (DiagShouldLog(site, &n))
    EOT_WARN("[draw] dropped: {} (x{})", why, n + 1);
}

bool ResolveTargets(VideoState &s, DeviceView dev, Targets &t) {
  t = Targets{};
  for (u32 i = 0; i < 4; ++i) {
    const u32 va = dev.U32(dev::kRenderTarget0 + 4 * i);
    if (!va)
      break;
    GuestSurface *surf = GetGuestSurface(s, va);
    if (!surf || !SurfaceHasImage(*surf))
      break;
    const u32 packet = dev.U32(i == 0 ? dev::kColor0Info : dev::kColor1Info + 4 * (i - 1));
    const u32 bias = (packet >> 20) & 0x3F;
    surf->colorExpBias = bias & 0x20 ? static_cast<i32>(bias) - 64 : static_cast<i32>(bias);
    t.color[i] = surf;
    t.colorImage[i] = surf->host.valid() ? &surf->host : &surf->single;
    t.colorCount = i + 1;
  }
  const u32 ds_va = dev.U32(dev::kDepthSurface);
  if (ds_va)
    t.depth = GetGuestSurface(s, ds_va);
  if (t.depth && !SurfaceHasImage(*t.depth))
    t.depth = nullptr;
  if (t.depth)
    t.depthImage = t.depth->host.valid() ? &t.depth->host : &t.depth->single;
  if (t.colorCount) {
    t.width = t.color[0]->width;
    t.height = t.color[0]->height;
  } else if (t.depth) {
    t.width = t.depth->width;
    t.height = t.depth->height;
  }
  // WoS port (fix49): Web of Shadows draws its alpha-tested shadow casters (trees, fences)
  // with the 1280x720 colour target 82f70cf8 still bound next to the 1024x1024 shadow map.
  // The colour target renders at the main scale (1.5), the shadow map at ShadowMapTargetScale
  // (2): taking the colour target's scale drew those casters into 75% of the shadow map (and
  // clipped to the colour target's size) - tree shadows floated away from the trees while
  // depth-only casters (characters) were right. That colour output is never resolved or
  // sampled; such draws go depth-only at the shadow map's scale (the pixel shader still runs
  // for alpha test).
  if (t.colorCount && t.depth && t.depth->isDepth && t.depth->width == 1024 &&
      t.depth->height == 1024 && t.depth->scale != t.color[0]->scale) {
    static u64 dropped = 0;
    if (dropped++ == 0)
      EOT_INFO("[shadow] colour target {}x{} (scale {}) bound with the shadow map {}x{} (scale {}): "
               "drawing depth-only at the shadow map's scale",
               t.color[0]->width, t.color[0]->height, t.color[0]->scale, t.depth->width,
               t.depth->height, t.depth->scale);
    for (u32 i = 0; i < 4; ++i) {
      t.color[i] = nullptr;
      t.colorImage[i] = nullptr;
    }
    t.colorCount = 0;
    t.width = t.depth->width;
    t.height = t.depth->height;
  }
  if (t.colorCount)
    t.scale = t.color[0]->scale;
  else if (t.depth)
    t.scale = t.depth->scale;
  t.samples = t.colorCount ? t.color[0]->host.sampleCount
                           : (t.depth ? t.depth->host.sampleCount : 1u);
  return t.colorCount || t.depth;
}

bool CaptureTargetWords(DeviceView dev, TargetWords &tw, bool for_clear = false) {
  tw.colorCount = 0;
  for (u32 i = 0; i < 4; ++i) {
    const u32 va = dev.U32(dev::kRenderTarget0 + 4 * i);
    if (!va || !ReadSurfaceHeaderWords(va, tw.colorWords[i]))
      break;
    tw.colorVa[i] = va;
    // WoS port (fix41): a clear of a predicated-tiling surface (BeginTiling's ClearF) reads
    // the tile's header, whose EDRAM word differs from the one the draws then use - a
    // different host surface: the scene's colour target was never cleared, and every frame
    // drew over the last (the "ghosting": stair-stepped buildings, trails of Spider-Man; the
    // fix40 [ghost] log: clear -> rt 0x82f70e08 uid 5, 1270 frames of draws -> uid 12). The
    // clear goes to the surface the draws use, as EndTiling's resolve already does.
    if (for_clear)
      wos_tiling::UseDrawWords(va, tw.colorWords[i]);
    else
      wos_tiling::NoteDrawWords(va, tw.colorWords[i]);  // WoS port: resolves read the same
    tw.colorInfo[i] = dev.U32(i == 0 ? dev::kColor0Info : dev::kColor1Info + 4 * (i - 1));
    tw.colorCount = i + 1;
  }
  tw.depthVa = dev.U32(dev::kDepthSurface);
  if (tw.depthVa && !ReadSurfaceHeaderWords(tw.depthVa, tw.depthWords))
    tw.depthVa = 0;
  if (tw.depthVa)
    wos_tiling::NoteDrawWords(tw.depthVa, tw.depthWords);
  return tw.colorCount || tw.depthVa;
}

bool ResolveTargetsFromWords(VideoState &s, const TargetWords &tw, Targets &t) {
  t = Targets{};
  for (u32 i = 0; i < tw.colorCount; ++i) {
    GuestSurface *surf = GetGuestSurfaceWords(s, tw.colorVa[i], tw.colorWords[i]);
    if (!surf || !SurfaceHasImage(*surf))
      break;
    const u32 bias = (tw.colorInfo[i] >> 20) & 0x3F;
    surf->colorExpBias = bias & 0x20 ? static_cast<i32>(bias) - 64 : static_cast<i32>(bias);
    t.color[i] = surf;
    t.colorImage[i] = surf->host.valid() ? &surf->host : &surf->single;
    t.colorCount = i + 1;
  }
  if (tw.depthVa)
    t.depth = GetGuestSurfaceWords(s, tw.depthVa, tw.depthWords);
  if (t.depth && !SurfaceHasImage(*t.depth))
    t.depth = nullptr;
  if (t.depth)
    t.depthImage = t.depth->host.valid() ? &t.depth->host : &t.depth->single;
  if (t.colorCount) {
    t.width = t.color[0]->width;
    t.height = t.color[0]->height;
  } else if (t.depth) {
    t.width = t.depth->width;
    t.height = t.depth->height;
  }
  // WoS port (fix49): Web of Shadows draws its alpha-tested shadow casters (trees, fences)
  // with the 1280x720 colour target 82f70cf8 still bound next to the 1024x1024 shadow map.
  // The colour target renders at the main scale (1.5), the shadow map at ShadowMapTargetScale
  // (2): taking the colour target's scale drew those casters into 75% of the shadow map (and
  // clipped to the colour target's size) - tree shadows floated away from the trees while
  // depth-only casters (characters) were right. That colour output is never resolved or
  // sampled; such draws go depth-only at the shadow map's scale (the pixel shader still runs
  // for alpha test).
  if (t.colorCount && t.depth && t.depth->isDepth && t.depth->width == 1024 &&
      t.depth->height == 1024 && t.depth->scale != t.color[0]->scale) {
    static u64 dropped = 0;
    if (dropped++ == 0)
      EOT_INFO("[shadow] colour target {}x{} (scale {}) bound with the shadow map {}x{} (scale {}): "
               "drawing depth-only at the shadow map's scale",
               t.color[0]->width, t.color[0]->height, t.color[0]->scale, t.depth->width,
               t.depth->height, t.depth->scale);
    for (u32 i = 0; i < 4; ++i) {
      t.color[i] = nullptr;
      t.colorImage[i] = nullptr;
    }
    t.colorCount = 0;
    t.width = t.depth->width;
    t.height = t.depth->height;
  }
  if (t.colorCount)
    t.scale = t.color[0]->scale;
  else if (t.depth)
    t.scale = t.depth->scale;
  t.samples = t.colorCount ? t.color[0]->host.sampleCount
                           : (t.depth ? t.depth->host.sampleCount : 1u);
  return t.colorCount || t.depth;
}

struct DrawClass {
  bool nullPs = false;
  bool depthTest = false;
  bool depthWrite = false;
  bool stencil = false;
  bool stencilWrite = false;
  bool blend = false;
  bool additive = false;
  bool rect = false;
  bool eightBit = false;
};

DrawClass ClassifyDraw(DeviceView dev, const Targets &t, bool has_ps, bool rect_list) {
  DrawClass c;
  c.nullPs = !has_ps;
  c.rect = rect_list;
  const u32 dc = dev.U32(dev::kDepthControl);
  const bool has_ds = t.depth != nullptr;
  c.depthTest = has_ds && (dc & 2);
  c.depthWrite = has_ds && (dc & 4);
  c.stencil = has_ds && (dc & 1);
  if (c.stencil) {
    const u32 front_ops = ((dc >> 11) & 7) | ((dc >> 14) & 7) | ((dc >> 17) & 7);
    const u32 back_ops = (dc & 0x80) ? (((dc >> 23) & 7) | ((dc >> 26) & 7) | ((dc >> 29) & 7)) : 0;
    const u32 write_mask = (dev.U32(dev::kStencilRefMask) >> 16) & 0xFF;
    c.stencilWrite = (front_ops | back_ops) != 0 && write_mask != 0;
  }
  if (t.colorCount) {
    const u32 cc = dev.U32(dev::kColorControl);
    const u32 bc = dev.U32(dev::kBlendControl0);
    const u32 src = bc & 0x1F, op = (bc >> 5) & 7, dst = (bc >> 8) & 0x1F;
    const bool passthrough = src == 1 && dst == 0 && op == 0;
    c.blend = !(cc & (1u << 5)) && !passthrough;
    c.additive = c.blend && op == 0 && dst == 1;
    c.eightBit = t.color[0]->colorFormat <= 1; // k_8_8_8_8, k_8_8_8_8_GAMMA
  }
  return c;
}

u32 SelectPassSamples(const Targets &t, const DrawClass &c, bool stencil_twin) {
  const u32 ms = t.colorCount ? t.color[0]->host.sampleCount
                              : (t.depth ? t.depth->host.sampleCount : 1u);
  if (ms <= 1)
    return 1;
  const bool ms_depth = t.depth && t.depth->host.sampleCount > 1;
  if (ms_depth && c.stencil && !stencil_twin)
    return ms;
  if (!t.colorCount)
    return ms;
  const GuestSurface &c0 = *t.color[0];
  const bool resolved = c0.content == GuestSurface::Content::Borrowed ||
                        (c0.content == GuestSurface::Content::Drawn &&
                         (c0.contentInSingle || c0.imagesAgree || c0.resolvedSinceDraw));
  if (c.nullPs) {
    if (ms_depth && resolved)
      return 1;
    return ms_depth && !c.depthWrite && stencil_twin ? 1u : ms;
  }
  if (resolved)
    return 1;
  if (ms_depth && c.depthWrite && !c.blend)
    return ms;
  if (!c.rect && !c.additive && !c.nullPs && c.depthTest && !c.blend && !c.eightBit)
    return ms;
  return c0.content == GuestSurface::Content::Drawn ? ms : 1u;
}

bool SelectTargetImages(VideoState &s, Targets &t, const DrawClass &c) {
  u32 samples = SelectPassSamples(t, c, s.stencil_ref_supported);
  if (t.depth && t.colorCount && samples > 1 && t.depth->host.sampleCount != samples)
    samples = 1;
  if (samples > 1) {
    for (u32 i = 0; i < t.colorCount && samples > 1; ++i)
      if (!t.color[i]->host.valid() && !SurfaceMakeMultisampled(s, *t.color[i]))
        samples = 1;
    if (samples > 1 && t.depth && t.depth->host.sampleCount == samples && !t.depth->host.valid() &&
        !SurfaceMakeMultisampled(s, *t.depth))
      samples = 1;
  }
  for (u32 i = 0; i < t.colorCount; ++i) {
    t.colorImage[i] = SurfaceImageForDraw(s, *t.color[i], samples, !c.nullPs);
    if (!t.colorImage[i])
      return false;
  }
  if (t.depth) {
    if (samples == t.depth->host.sampleCount) {
      if (samples > 1 && t.depth->singleDirty && !SurfacePropagateDepthSingle(s, *t.depth))
        return false;
      t.depthImage = &t.depth->host;
      if (!t.colorCount && t.depth->redirectMirror && t.depth->redirectMirror->host.valid()) {
        t.depthImage = &t.depth->redirectMirror->host;
        t.offsetX = t.depth->redirectX;
        t.offsetY = t.depth->redirectY;
      }
    } else {
      t.depthImage = SurfaceDepthSingle(s, *t.depth, c.depthTest || c.stencil || c.depthWrite);
      if (!t.depthImage)
        return false;
    }
  }
  t.samples = samples;
  t.writesDepthStencil = c.depthWrite || (c.stencilWrite && s.stencil_ref_supported);
  t.writesDepth = c.depthWrite;
  t.writesColor = !c.nullPs;
  t.additive = c.additive;
  return true;
}

bool BindImages(VideoState &s, HostTexture *const colors[4], u32 color_count, HostTexture *depth,
                bool init_clear) {
  HostTextureTransition transitions[kMaxPendingTransitions + 5];
  u32 transition_count = 0;
  for (u32 i = 0; i < s.pending_transition_count; ++i)
    transitions[transition_count++] = s.pending_transitions[i];
  s.pending_transition_count = 0;
  for (u32 i = 0; i < color_count; ++i)
    transitions[transition_count++] = {colors[i], plume::RenderTextureLayout::COLOR_WRITE};
  if (depth)
    transitions[transition_count++] = {depth, plume::RenderTextureLayout::DEPTH_WRITE};
  TransitionManyLocked(s, transitions, transition_count);
  if (depth && color_count && colors[0]->sampleCount != depth->sampleCount) {
    u32 n;
    if (DiagShouldLog(0x6C20, &n) && n < 8)
      EOT_WARN("[draw] colour {}x{} ({}x samples) bound with depth {}x{} ({}x samples)",
               colors[0]->width, colors[0]->height, colors[0]->sampleCount, depth->width,
               depth->height, depth->sampleCount);
  }
  bool same_targets = s.bound_draw_targets_valid && s.bound_framebuffer &&
                      s.bound_draw_color_count == color_count &&
                      s.bound_draw_depth == (depth ? depth->texture.get() : nullptr);
  for (u32 i = 0; same_targets && i < color_count; ++i)
    same_targets = s.bound_draw_colors[i] == colors[i]->texture.get();
  if (same_targets) {
    s.perf.framebuffer_cache_hits++;
  } else {
    plume::RenderFramebuffer *fb = GetFramebuffer(s, colors, color_count, depth);
    if (!fb)
      return false;
    if (s.bound_framebuffer != fb) {
      s.command_list->setFramebuffer(fb);
      s.bound_framebuffer = fb;
    }
    s.bound_draw_color_count = color_count;
    for (u32 i = 0; i < 4; ++i)
      s.bound_draw_colors[i] = i < color_count ? colors[i]->texture.get() : nullptr;
    s.bound_draw_depth = depth ? depth->texture.get() : nullptr;
    s.bound_draw_targets_valid = true;
  }
  if (!init_clear)
    return true;
  for (u32 i = 0; i < color_count; ++i) {
    if (!colors[i]->needsClear)
      continue;
    s.command_list->clearColor(i, plume::RenderColor(0, 0, 0, 0), nullptr, 0);
    colors[i]->needsClear = false;
  }
  if (depth && depth->needsClear) {
    s.command_list->clearDepthStencil(true, FormatHasStencil(depth->format), 0.0f, 0, nullptr, 0);
    depth->needsClear = false;
  }
  return true;
}

bool BindTargets(VideoState &s, Targets &t) {
  HostTexture *colors[4] = {};
  for (u32 i = 0; i < t.colorCount; ++i) {
    colors[i] = t.colorImage[i] ? t.colorImage[i] : &t.color[i]->host;
    if (t.writesColor)
      NoteSurfaceDrawn(*t.color[i], *colors[i], false);
    t.color[i]->lastUseFrame = s.guest_frames;
  }
  HostTexture *depth = nullptr;
  if (t.depth) {
    depth = t.depthImage ? t.depthImage : &t.depth->host;
    NoteSurfaceDrawn(*t.depth, *depth, t.writesDepthStencil, t.writesDepth);
    t.depth->lastUseFrame = s.guest_frames;
  }
  u32 color_count = t.colorCount;
  if (t.velocity && color_count < 4)
    colors[color_count++] = &t.velocity->image;
  if (!BindImages(s, colors, color_count, depth, true))
    return false;
  if (t.velocity)
    velocity::BeforeDraw(s, *t.velocity, color_count - 1);
  GpuTimingMark(s, s.command_list,
                GpuTargetCategory(t.width == kGuestRenderWidth && t.height == kGuestRenderHeight,
                                  t.depth != nullptr, t.colorCount,
                                  t.colorCount ? static_cast<u32>(colors[0]->format) : 0u,
                                  t.samples, t.additive));
  return true;
}

// WoS port (fix40): the "ghosting" (geometry smeared across the sky, stair-stepped copies of
// Spider-Man) looks like the scene's colour/depth carried over from earlier frames. Per frame:
// which surfaces were cleared (and how), and the surface most drawn into; when the most-drawn
// colour target or its depth had no whole clear that frame, log both sides.
namespace ghost {
struct ClearNote {
  u32 va;
  u64 uid;
  u32 w, h, flags;
  bool whole, depth;
};
struct State {
  u64 frame = ~0ull;
  std::vector<ClearNote> clears;
  std::unordered_map<u64, u32> draws;  // colour uid -> draws
  std::unordered_map<u64, std::pair<u32, u64>> depth_of;  // colour uid -> (depth va, depth uid)
  std::unordered_map<u64, u32> va_of;
  std::unordered_map<u64, std::pair<u32, u32>> size_of;
  u64 reported = 0;
};
State &st() {
  static State g;
  return g;
}
void Roll(u64 frame) {
  State &g = st();
  if (g.frame == frame)
    return;
  if (g.frame != ~0ull && !g.draws.empty()) {
    u64 top = 0;
    u32 n = 0;
    for (auto &[uid, c] : g.draws)
      if (c > n) {
        n = c;
        top = uid;
      }
    const auto dep = g.depth_of[top];
    bool color_clear = false, depth_clear = false;
    for (const auto &c : g.clears) {
      if (c.whole && !c.depth && c.uid == top)
        color_clear = true;
      if (c.whole && c.depth && c.uid == dep.second)
        depth_clear = true;
    }
    if (n >= 150 && (!color_clear || !depth_clear)) {
      ++g.reported;
      if (g.reported <= 40 || g.reported % 300 == 0) {
        std::string cl;
        for (const auto &c : g.clears)
          cl += std::format(" [{} {:#x} uid {} {}x{} flags {:#x} {}]", c.depth ? "ds" : "rt", c.va,
                            c.uid, c.w, c.h, c.flags, c.whole ? "whole" : "rect");
        EOT_WARN("[ghost] frame {}: {} draws into rt {:#x} uid {} {}x{} (ds {:#x} uid {}) - colour "
                 "cleared {}, depth cleared {} (x{}); clears this frame:{}",
                 g.frame, n, g.va_of[top], top, g.size_of[top].first, g.size_of[top].second,
                 dep.first, dep.second, color_clear, depth_clear, g.reported, cl.empty() ? " none" : cl);
      }
    }
  }
  g.frame = frame;
  g.clears.clear();
  g.draws.clear();
  g.depth_of.clear();
}
void NoteDraw(u64 frame, const Targets &t) {
  if (!t.colorCount || !t.color[0])
    return;
  Roll(frame);
  State &g = st();
  const u64 uid = t.color[0]->uid;
  ++g.draws[uid];
  g.va_of[uid] = t.color[0]->va;
  g.size_of[uid] = {t.width, t.height};
  if (t.depth)
    g.depth_of[uid] = {t.depth->va, t.depth->uid};
}
void NoteClear(u64 frame, const Targets &t, u32 flags, bool whole) {
  Roll(frame);
  State &g = st();
  for (u32 i = 0; i < t.colorCount; ++i)
    if (t.color[i] && (flags & (1u << i)))
      g.clears.push_back({t.color[i]->va, t.color[i]->uid, t.width, t.height, flags, whole, false});
  if (t.depth && (flags & 0x10))
    g.clears.push_back({t.depth->va, t.depth->uid, t.width, t.height, flags, whole, true});
}
}  // namespace ghost

ViewportInfo ComputeViewport(DeviceView dev, const Targets &t, float jitter_x, float jitter_y) {
  ViewportInfo v;
  const float rt_w = static_cast<float>(std::max(1u, t.width));
  const float rt_h = static_cast<float>(std::max(1u, t.height));
  const u32 vte = dev.U32(dev::kVteControl);
  const float xs = dev.F32(dev::kVportXScale), xo = dev.F32(dev::kVportXOffset);
  const float ys = dev.F32(dev::kVportYScale), yo = dev.F32(dev::kVportYOffset);
  const float zs = dev.F32(dev::kVportZScale), zo = dev.F32(dev::kVportZOffset);
  const bool xs_en = vte & 1, xo_en = vte & 2, ys_en = vte & 4, yo_en = vte & 8;
  const bool zs_en = vte & 16, zo_en = vte & 32;

  float x0, w, y0, h;
  if (xs_en && xs != 0.0f) {
    w = 2.0f * std::fabs(xs);
    x0 = (xo_en ? xo : 0.0f) - std::fabs(xs);
    v.posScale[0] = xs < 0 ? -1.0f : 1.0f;
  } else {
    x0 = 0.0f;
    w = rt_w;
    v.posScale[0] = 2.0f / rt_w;
    v.posOffset[0] = (xo_en ? xo : 0.0f) * 2.0f / rt_w - 1.0f;
  }
  if (ys_en && ys != 0.0f) {
    h = 2.0f * std::fabs(ys);
    y0 = (yo_en ? yo : 0.0f) - std::fabs(ys);
    v.posScale[1] = ys < 0 ? 1.0f : -1.0f;
  } else {
    y0 = 0.0f;
    h = rt_h;
    v.posScale[1] = -2.0f / rt_h;
    v.posOffset[1] = 1.0f - (yo_en ? yo : 0.0f) * 2.0f / rt_h;
  }
  float zmin, zmax;
  if (zs_en) {
    if (zs >= 0.0f) {
      zmin = zo_en ? zo : 0.0f;
      zmax = zmin + zs;
    } else {
      zmax = zo_en ? zo : 0.0f;
      zmin = zmax + zs;
      v.posScale[2] = -1.0f;
      v.posOffset[2] = 1.0f;
    }
  } else {
    zmin = 0.0f;
    zmax = 1.0f;
    v.posOffset[2] = zo_en ? zo : 0.0f;
  }
  zmin = std::clamp(zmin, 0.0f, 1.0f);
  zmax = std::clamp(zmax, 0.0f, 1.0f);
  if (dev.U32(dev::kModeControl) & (1u << 11)) {
    const bool scene = t.colorCount > 0 || (t.width == kGuestRenderWidth && t.height == kGuestRenderHeight);
    const float offset = dev.F32(dev::kPolyOffsetFrontOffset);
    // WoS port (fix87): wos_debug_no_zshift - the bias is also in the pipeline's depthBias;
    // this extra shift of the vertex z is EoT's and can push a draw out of [0,1] (clipped)
    static const bool no_zshift = wos_debug_no_zshift();
    if (!no_zshift && scene && offset != 0.0f && std::isfinite(offset) && zmax > zmin)
      v.posOffset[2] += offset / (zmax - zmin);
  }
  if ((dev.U32(dev::kVtxControl) & 1) == 0) {
    // WoS port (fix40): D3D9's pixel centres (PA_SU_VTX_CNTL.PIX_CENTER = 0) by half a HOST
    // pixel, not half a guest one: at 1.5x a guest half pixel is 0.75 host pixels, which moved
    // full-screen quads drawn from x = 0 past the first column's sample point - the loading
    // screen's black cover left the scene showing in a 1-pixel strip down the left edge (and
    // along the top). At 1x the two are the same.
    const float sc = std::max(1.0f, t.scale);
    v.posOffset[0] += 1.0f / std::max(1.0f, w * sc);
    v.posOffset[1] -= 1.0f / std::max(1.0f, h * sc);
  }
  if ((jitter_x != 0.0f || jitter_y != 0.0f) && xs_en && ys_en && t.width == kGuestRenderWidth &&
      t.height == kGuestRenderHeight) {
    v.posOffset[0] += jitter_x * 2.0f / std::max(1.0f, w * t.scale);
    v.posOffset[1] -= jitter_y * 2.0f / std::max(1.0f, h * t.scale);
  }
  const float S = t.scale;
  const float hx0 = std::round(x0 * S), hx1 = std::round((x0 + w) * S);
  const float hy0 = std::round(y0 * S), hy1 = std::round((y0 + h) * S);
  v.vp = plume::RenderViewport(hx0, hy0, std::max(hx1 - hx0, 1.0f), std::max(hy1 - hy0, 1.0f),
                               zmin, zmax);

  auto scissor_of = [](u32 tl, u32 br) {
    return plume::RenderRect(static_cast<i32>(tl & 0x7FFF), static_cast<i32>((tl >> 16) & 0x7FFF),
                             static_cast<i32>(br & 0x7FFF), static_cast<i32>((br >> 16) & 0x7FFF));
  };
  const plume::RenderRect win =
      scissor_of(dev.U32(dev::kWindowScissorTL), dev.U32(dev::kWindowScissorBR));
  const plume::RenderRect scr =
      scissor_of(dev.U32(dev::kScreenScissorTL), dev.U32(dev::kScreenScissorBR));
  plume::RenderRect sc;
  sc.left = std::max({0, win.left, scr.left});
  sc.top = std::max({0, win.top, scr.top});
  sc.right = std::min({static_cast<i32>(t.width), win.right, scr.right});
  sc.bottom = std::min({static_cast<i32>(t.height), win.bottom, scr.bottom});
  if (sc.right <= sc.left || sc.bottom <= sc.top)
    sc = plume::RenderRect(0, 0, static_cast<i32>(t.width), static_cast<i32>(t.height));
  sc = plume::RenderRect(ScalePxBy(sc.left, S), ScalePxBy(sc.top, S),
                         std::max(ScalePxBy(sc.right, S), ScalePxBy(sc.left, S) + 1),
                         std::max(ScalePxBy(sc.bottom, S), ScalePxBy(sc.top, S) + 1));
  if (t.offsetX || t.offsetY) {
    v.vp.x += static_cast<float>(t.offsetX);
    v.vp.y += static_cast<float>(t.offsetY);
    sc.left += t.offsetX;
    sc.right += t.offsetX;
    sc.top += t.offsetY;
    sc.bottom += t.offsetY;
  }
  v.scissor = sc;
  return v;
}

struct ConstFileCache {
  alignas(16) u8 bytes[256 * 16] = {};
  UploadAlloc alloc;
  u64 epoch = ~0ull;
  u32 regs = 0;
  bool allocRaw = false;  // fix111: alloc holds raw guest bytes (+ a swapped marker before it)
};
// fix111: a raw (render-thread swapped) constant upload has this many bytes in front of it;
// the first word is 0 until the render thread has byte-swapped the file, then 1
constexpr u64 kRawConstHeader = 256;

extern "C" bool wos_stack_garbage_zero();
extern "C" bool wos_stale_alias_white();

bool UploadFloatFile(VideoState &s, DeviceView dev, u32 offset, u32 stage, u32 regs,
                     u64 dirty, UploadAlloc *out, bool *raw_out = nullptr) {
  if (raw_out)
    *raw_out = false;
  static ConstFileCache caches[2];
  static const bool verify = Settings::FastSettersVerify();
  regs = std::clamp(regs, 16u, 256u);
  const u32 bytes = regs * 16;
  const u8 *src = dev.Bytes(offset, bytes);
  if (!src)
    return false;
  ConstFileCache &c = caches[stage & 1];
  const u32 blocks = (bytes + 63) / 64;
  const u64 in_file = blocks >= 64 ? ~0ull : ((1ull << blocks) - 1);
  auto compare_blocks = [&](u64 mask) {
    u64 changed = 0;
    for (u64 m = mask & in_file; m; m &= m - 1) {
      const u32 b = static_cast<u32>(std::countr_zero(m));
      const u32 n = std::min(64u, bytes - 64 * b);
      if (std::memcmp(c.bytes + 64 * b, src + 64 * b, n) != 0)
        changed |= 1ull << b;
    }
    return changed;
  };
  u64 refresh = ~0ull;
  if (c.epoch == UploadRingEpoch() && c.regs >= regs) {
    // WoS port: the pending mask is read before the game's own draw runs, and that draw's
    // state flush (sub_82AC1D40) writes the new shader's and material's constants into
    // the shadow without a pending bit - trusting the mask kept the previous material's
    // constants for the first draw of every new one (fog, sky, light parameters from
    // another shader: the white, red and green washes). Every block is compared instead.
    (void)dirty;
    // WoS port (fix51): one whole-range compare. compare_blocks made a memcmp call per 64-byte
    // block (up to 64 per file, two files a draw): ~8 ms of the game thread's ~13 ms capture
    // per city frame (fix50 [perf]: const float 7.98 ms, 2600 draws). A changed file is
    // uploaded whole anyway, so the per-block mask only steered the cache refresh below -
    // a changed file now refreshes the whole cached copy (one memcpy).
    u64 changed = std::memcmp(c.bytes, src, bytes) != 0 ? ~0ull : 0;
    if (verify) {
      const u64 actual = compare_blocks(~0ull);
      if (actual & ~dirty) {
        static u32 logged = 0;
        if (logged++ < 24)
          EOT_WARN("[draw] float file {} changed outside its pending mask: mask {:#018x} changed {:#018x} "
                   "(regs {})",
                   stage, dirty, actual, regs);
        s.perf.const_file_mask_misses++;
        changed |= actual;
      }
    }
    if (!changed && c.allocRaw && !raw_out) {
      changed = ~0ull;  // fix111: a raw upload is no use to a caller that cannot defer the swap
    }
    if (!changed) {
      *out = c.alloc;
      if (raw_out)
        *raw_out = c.allocRaw;
      c.regs = regs;
      s.perf.const_file_hits++;
      return true;
    }
    refresh = ~0ull;
  }
  // WoS port (fix111): wos_defer_constants - a plain copy here; the render thread byte-swaps it
  // (and applies the fix100 0xBE fix-up) once, before the first draw that uses it replays
  if (raw_out) {
    if (!UploadAllocate(bytes + kRawConstHeader, kConstantBufferAlignment, out))
      return false;
    std::memset(out->cpu, 0, 4);
    out->offset += kRawConstHeader;
    out->cpu += kRawConstHeader;
    out->size = bytes;
    if (out->gpuVa)
      out->gpuVa += kRawConstHeader;
    std::memcpy(out->cpu, src, bytes);
    *raw_out = true;
  } else {
  if (!UploadAllocate(bytes, kConstantBufferAlignment, out))
    return false;
  rex::memory::copy_and_swap_32_unaligned(out->cpu, reinterpret_cast<const u32 *>(src), regs * 4);
  // WoS port (fix100): the terrain's SetVertexShaderConstantF(0, 50) passes a stack array
  // whose c27/c28 (horizon_map_matrix_u/v) it never fills, and its pixel constants c23/c38
  // likewise. rexglue fills every guest thread stack with 0xBE, so those registers held
  // 0xBEBEBEBE (-0.3725) - the dark/black cement square. The console's (and the emulated
  // path's) stack held leftovers there, never this fill pattern: upload it as 0.
  if (wos_stack_garbage_zero()) {
    auto *w = reinterpret_cast<u32 *>(out->cpu);
    u32 fixed = 0;
    for (u32 i = 0, n = regs * 4; i < n; ++i)
      if (w[i] == 0xBEBEBEBEu) {
        w[i] = 0;
        ++fixed;
      }
    if (fixed) {
      static std::atomic<u32> logged{0};
      if (logged.fetch_add(1, std::memory_order_relaxed) < 16)
        EOT_INFO("[draw] fix100: {} {} constant dword(s) were the 0xBE stack fill -> 0",
                 fixed, stage ? "pixel" : "vertex");
    }
  }
  }  // fix111: !raw_out
  if (refresh == ~0ull) {
    std::memcpy(c.bytes, src, bytes);
  } else {
    for (u64 m = refresh & in_file; m; m &= m - 1) {
      const u32 b = static_cast<u32>(std::countr_zero(m));
      std::memcpy(c.bytes + 64 * b, src + 64 * b, std::min(64u, bytes - 64 * b));
    }
  }
  c.alloc = *out;
  c.allocRaw = raw_out && *raw_out;
  c.epoch = UploadRingEpoch();
  c.regs = regs;
  s.perf.constant_bytes += bytes;
  return true;
}

void FillLoopConstants(DeviceView dev, u32 offset, i32 (*dst)[4]) {
  for (u32 i = 0; i < 16; ++i) {
    const u32 v = dev.U32(offset + 4 * i);
    dst[i][0] = static_cast<i32>(v & 0xFF);
    dst[i][1] = static_cast<i32>((v >> 8) & 0xFF);
    dst[i][2] = static_cast<i32>(static_cast<i8>((v >> 16) & 0xFF));
    dst[i][3] = 0;
  }
}

struct GeometryPlan {
  const CachedIndexRange *cached = nullptr;
  plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  bool indexed = false;
  std::vector<u32> indices;
  u32 vertexCount = 0;
  u32 startVertex = 0;
  u32 minVertex = 0;
  u32 maxVertex = 0;
  i32 baseVertex = 0;
  bool rectList = false;
  u32 stream0OverrideVa = 0;
  u32 stream0OverrideStride = 0;
  u32 startIndex = 0;  // (fix89) the guest start index and count, for the ground probe
  u32 guestIndexCount = 0;
  // (fix117) a quad list drawn with the shared index pattern: no index list of its own
  bool quadPattern = false;
};

bool ReadGuestIndices(u32 ib_va, u32 start_index, u32 count, std::vector<u32> &out) {
  if (!ib_va || !count)
    return false;
  const u32 common = mem::load<u32>(ib_va + obj::kCommon);
  const u32 base = mem::load<u32>(ib_va + obj::kBufferFetch0);
  const u32 size = mem::load<u32>(ib_va + obj::kBufferFetch1);
  {
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
      EOT_INFO("[draw] index buffer {:#x}: data {:#x} ({}) size {} common {:#x}", ib_va, base,
               base < 0x20000000u ? "physical" : "virtual", size, common);
  }
  const bool is32 = (common & obj::kIndexBuffer32BitBit) != 0;
  const u32 elem = is32 ? 4 : 2;
  if (!base)
    return false;
  if (size && (u64(start_index) + count) * elem > size) {
    u32 n;
    if (DiagShouldLog(0x6100, &n))
      EOT_WARN("[draw] index range {}+{} exceeds buffer {:#x} size {}", start_index, count, ib_va,
               size);
    count = static_cast<u32>(size / elem) > start_index ? size / elem - start_index : 0;
    if (!count)
      return false;
  }
  out.resize(count);
  if (is32) {
    auto *p = mem::gpu<be_u32>(base + start_index * 4);  // WoS port: physical
    if (!p)
      return false;
    for (u32 i = 0; i < count; ++i)
      out[i] = p[i];
  } else {
    auto *p = mem::gpu<be_u16>(base + start_index * 2);
    if (!p)
      return false;
    for (u32 i = 0; i < count; ++i)
      out[i] = p[i];
  }
  return true;
}

void ExpandIndices(u32 prim, std::vector<u32> &idx, bool has_restart) {
  std::vector<u32> out;
  switch (prim) {
  case kPrimTriangleStrip: {
    out.reserve(idx.size() * 3);
    u32 run = 0;
    for (size_t i = 0; i < idx.size(); ++i) {
      if (has_restart && (idx[i] == 0xFFFF || idx[i] == 0xFFFFFFFFu)) {
        run = 0;
        continue;
      }
      ++run;
      if (run >= 3) {
        const u32 a = idx[i - 2], b = idx[i - 1], c = idx[i];
        if ((run & 1) == 1) {
          out.push_back(a);
          out.push_back(b);
          out.push_back(c);
        } else {
          out.push_back(b);
          out.push_back(a);
          out.push_back(c);
        }
      }
    }
    break;
  }
  case kPrimTriangleFan: {
    out.reserve(idx.size() * 3);
    for (size_t i = 2; i < idx.size(); ++i) {
      out.push_back(idx[0]);
      out.push_back(idx[i - 1]);
      out.push_back(idx[i]);
    }
    break;
  }
  case kPrimQuadList: {
    out.reserve(idx.size() / 4 * 6);
    for (size_t i = 0; i + 3 < idx.size(); i += 4) {
      out.push_back(idx[i]);
      out.push_back(idx[i + 1]);
      out.push_back(idx[i + 2]);
      out.push_back(idx[i]);
      out.push_back(idx[i + 2]);
      out.push_back(idx[i + 3]);
    }
    break;
  }
  case kPrimLineStrip: {
    out.reserve(idx.size() * 2);
    for (size_t i = 1; i < idx.size(); ++i) {
      if (has_restart && (idx[i] == 0xFFFF || idx[i - 1] == 0xFFFF))
        continue;
      out.push_back(idx[i - 1]);
      out.push_back(idx[i]);
    }
    break;
  }
  default:
    return;
  }
  idx.swap(out);
}

struct StreamInfo {
  u32 stream = 0;
  u32 stride = 0;
  const u8 *data = nullptr;
  u32 sizeBytes = 0;
  u32 dataVa = 0;
  u32 objectVa = 0;
};

bool ReadStream(DeviceView dev, u32 stream, StreamInfo &out) {
  out.stream = stream;
  out.stride = dev.U8(dev::kStreamStride0 + stream) * 4u;
  const u32 slot = dev::StreamFetchSlotOffset(stream);
  const u32 d0 = dev.U32(slot);
  const u32 d1 = dev.U32(slot + 4);
  if ((d0 & 3) != 3 || !out.stride)
    return false;
  out.data = mem::phys<u8>(d0 & ~3u);
  out.sizeBytes = ((d1 >> 2) & 0xFFFFFF) * 4u;
  out.dataVa = d0 & ~3u;
  out.objectVa = dev.U32(dev::kStreamObject0 + 4 * stream);
  return out.data != nullptr;
}

void CopyVertexBytes(u8 *dst, const u8 *src, u32 bytes) {
  const u32 dwords = bytes / 4;
  rex::memory::copy_and_swap_32_unaligned(dst, src, dwords);
  if (bytes & 3)
    std::memcpy(dst + dwords * 4, src + dwords * 4, bytes & 3);
}

struct IndexCacheKey {
  u32 ib_va = 0, data_va = 0, start = 0, count = 0, prim = 0;
  u64 seq = 0, sample = 0;
  bool operator==(const IndexCacheKey &o) const {
    return ib_va == o.ib_va && data_va == o.data_va && start == o.start && count == o.count &&
           prim == o.prim && seq == o.seq && sample == o.sample;
  }
};
struct IndexCacheKeyHash {
  size_t operator()(const IndexCacheKey &k) const {
    u64 h = 0x9E3779B97F4A7C15ull;
    auto mix = [&](u64 v) {
      h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      h *= 0xFF51AFD7ED558CCDull;
    };
    mix(k.ib_va);
    mix(k.data_va);
    mix((u64(k.start) << 32) | k.count);
    mix(k.prim);
    mix(k.seq);
    mix(k.sample);
    return static_cast<size_t>(h);
  }
};
struct IndexCacheChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  std::unique_ptr<plume::RenderBuffer> vram;
  u64 vramValidUpTo = 0;
  u64 sealed = 0;
  u8 *cpu = nullptr;
  u64 capacity = 0, used = 0;
  u64 accounted = 0;
  u64 lastUseFrame = 0;
};
struct IndexCache {
  std::unordered_map<IndexCacheKey, CachedIndexRange, IndexCacheKeyHash> map;
};
IndexCache &index_cache() {
  static IndexCache c;
  return c;
}
constexpr u64 kIndexCacheChunkBytes = 8ull << 20;
constexpr u64 kIndexCacheBudgetBytes = 96ull << 20;

u64 SampleHostBytes(const u8 *p, u32 bytes) {
  if (!p || bytes < 4)
    return 0;
  // WoS port (fix39): the fingerprint the vertex/index caches key ranges by. EoT sampled the
  // first and last 16 bytes only - enough for Prince of Persia, which Locks/Unlocks what it
  // rewrites (the key's unlock serial catches it). Web of Shadows rewrites dynamic buffers in
  // place without an Unlock, so a range whose ends stayed put kept its stale cached copy:
  // single frames of exploded meshes, black spikes and a wrong-texture menu banner. Small
  // ranges (UI quads, most dynamic geometry) are hashed whole; larger ones at their ends and
  // 14 evenly spaced 16-byte probes between.
  if (bytes <= 4096)
    return XXH3_64bits(p, bytes);
  u64 h = 1469598103934665603ull;
  auto mix = [&](u32 off) {
    u32 v;
    std::memcpy(&v, p + off, 4);
    h ^= v;
    h *= 1099511628211ull;
  };
  const u32 span = bytes - 16;
  for (u32 k = 0; k < 16; ++k) {
    const u32 base = static_cast<u32>((u64(span) * k / 15) & ~3ull);
    for (u32 i = 0; i < 16 && base + i + 4 <= bytes; i += 4)
      mix(base + i);
  }
  return h ^ bytes;
}
u64 SampleGuestBytes(u32 va, u32 bytes) { return SampleHostBytes(mem::gpu<u8>(va), bytes); }

void PrefetchSampleProbes(const u8 *p, u64 bytes) {
  if (!p || bytes < 4)
    return;
  eot::cpu::Prefetch(p);
  eot::cpu::Prefetch(p + 15);
  if (bytes >= 16) {
    const u64 span = (bytes - 16) & ~3ull;
    eot::cpu::Prefetch(p + span);
    eot::cpu::Prefetch(p + span + 15);
  }
}

}

void PrefetchIndexProbes(u32 device_va, u32 start_index, u32 index_count) {
  if (!device_va || !index_count)
    return;
  const u8 *dev = mem::at<u8>(device_va);
  if (!dev)
    return;
  const u32 ib_va = rex::memory::load_and_swap<u32>(dev + dev::kIndexBuffer);
  const u8 *ib = ib_va ? mem::at<u8>(ib_va) : nullptr;
  if (!ib)
    return;
  const u32 common = rex::memory::load_and_swap<u32>(ib + obj::kCommon);
  const u32 data_va = rex::memory::load_and_swap<u32>(ib + obj::kBufferFetch0);
  const u32 size = rex::memory::load_and_swap<u32>(ib + obj::kBufferFetch1);
  if (!data_va)
    return;
  const u32 elem = (common & obj::kIndexBuffer32BitBit) ? 4 : 2;
  const u64 start = u64(start_index) * elem;
  u64 bytes = u64(index_count) * elem;
  if (size && start + bytes > size)
    bytes = start < size ? size - start : 0;
  if (start > 0xFFFFFFFFu - data_va)
    return;
  PrefetchSampleProbes(mem::gpu<u8>(data_va + static_cast<u32>(start)), bytes);
}

// WoS port (fix108): the engine's shader-instanced meshes. The VS splits the vertex index:
// instance = trunc(index / instance_params.y), mesh vertex = index - instance * N, and fetches
// the mesh stream (POSITION0, uv) with the mesh vertex and the instance stream (POSITION1-4 =
// the instance's world matrix rows, COLOR0) with the instance. XenosRecomp turns every vfetch
// into a per-vertex input and drops that index maths, so each vertex read instance record
// #index: a garbage matrix per vertex - the huge flashing white / green shapes over the
// first-act daytime city (vs 244b6e2f / ps 6d386ade). Here the two streams are re-laid
// per guest vertex on the CPU: record j = mesh[h % N] / instance[h / N], h = lo + j.
extern "C" bool wos_instancing();
extern "C" bool wos_defer_vertices();
extern "C" bool wos_defer_constants();
extern "C" bool wos_defer_verify();
static bool wos_defer_vertices_cached() {
  static const bool on = wos_defer_vertices();
  return on;
}
namespace {
// streams read with the instance index (0 = not an instancer)
u32 InstanceStreamMask(const InputLayout &layout, u64 vs_hash) {
  u32 rows_on[16] = {};
  u32 pos0_stream = 16;
  for (const auto &e : layout.elements) {
    if (!e.semanticName || e.slotIndex >= 16 || std::string_view(e.semanticName) != "POSITION")
      continue;
    const u32 idx = e.semanticIndex & 0xFF;
    if (idx == 0)
      pos0_stream = e.slotIndex;
    else if (idx >= 1 && idx <= 4)
      rows_on[e.slotIndex] |= 1u << idx;
  }
  u32 mask = 0;
  for (u32 S = 0; S < 16; ++S)
    if (rows_on[S] == 0x1Eu && S != pos0_stream)
      mask |= 1u << S;
  // (seen in the first act: vs 244b6e2f and e5320f0f, both with instance_params at c6)
  (void)vs_hash;
  if (!mask || pos0_stream >= 16)
    return 0;
  return mask;
}
}  // namespace

u64 DrawSortKey(const DrawPacket &pk, u32 *depth_func) {
  if (pk.rectList || !pk.vs)
    return 0;
  DeviceView dev = Device(pk.device_va);
  dev.snapshot = pk.window.image;
  dev.snapshotSize = kDeviceSnapshotBytes;
  dev.pages = DeviceWindow::PageTable();
  const u32 dc = dev.U32(dev::kDepthControl);
  if (!(dc & 2))
    return 0;
  if (dc & 1) {
    auto constant_write = [](u32 func, u32 fail, u32 zpass, u32 zfail) {
      auto ok = [](u32 op) { return op == 0 || op == 1 || op == 2; };
      return func == 7 && ok(fail) && ok(zpass) && ok(zfail);
    };
    if (!constant_write((dc >> 8) & 7, (dc >> 11) & 7, (dc >> 14) & 7, (dc >> 17) & 7))
      return 0;
    if ((dc & 0x80) &&
        !constant_write((dc >> 20) & 7, (dc >> 23) & 7, (dc >> 26) & 7, (dc >> 29) & 7))
      return 0;
  }
  const u32 zfunc = (dc >> 4) & 7;
  const bool ordered = zfunc == 1 || zfunc == 3 || zfunc == 4 || zfunc == 6;
  if (!(dc & 4) || !ordered)
    return 0;
  if (depth_func)
    *depth_func = zfunc;
  const u32 cc = dev.U32(dev::kColorControl), bc = dev.U32(dev::kBlendControl0);
  const u32 src = bc & 0x1F, op = (bc >> 5) & 7, dst = (bc >> 8) & 0x1F;
  const bool passthrough = src == 1 && dst == 0 && op == 0;
  if (!(cc & (1u << 5)) && !passthrough)
    return 0;
  u64 h = 0x9E3779B97F4A7C15ull;
  auto mix = [&](u64 v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xFF51AFD7ED558CCDull;
  };
  mix(reinterpret_cast<u64>(pk.vs));
  mix(reinterpret_cast<u64>(pk.ps));
  mix(reinterpret_cast<u64>(pk.layout));
  mix(static_cast<u64>(pk.topology));
  for (u32 S = 0; S <= pk.max_slot && S < 16; ++S)
    mix(pk.strides[S]);
  constexpr u32 kRegs[] = {dev::kModeControl,   dev::kDepthControl, dev::kBlendControl0,
                           dev::kBlendControl1, dev::kBlendControl2, dev::kBlendControl3,
                           dev::kColorControl,  dev::kColorMask,     dev::kAlphaRef};
  for (u32 off : kRegs)
    mix(dev.U32(off));
  mix(dev.U32(dev::kStencilRefMask));
  return h ? h : 1;
}

namespace {

struct BufferPool {
  std::mutex mutex;
  std::vector<IndexCacheChunk> chunks;
  u64 totalBytes = 0;
  std::vector<std::unique_ptr<plume::RenderBuffer>> retired;
  std::atomic<bool> hasRetired{false};
  std::atomic<u64> flushEpoch{1};
};
void PoolDrainRetired(VideoState &s, BufferPool &pool) {
  if (!pool.hasRetired.load(std::memory_order_acquire))
    return;
  std::vector<std::unique_ptr<plume::RenderBuffer>> retired;
  {
    std::lock_guard lock(pool.mutex);
    pool.hasRetired.store(false, std::memory_order_relaxed);
    if (pool.retired.empty())
      return;
    retired.swap(pool.retired);
  }
  for (auto &b : retired) {
    if (!b)
      continue;
    if (RenderThreadActive()) {
      RenderThreadRetire(b.release());
    } else {
      std::lock_guard video_lock(s.mutex);
      ParkBuffer(s, std::move(b));
    }
  }
}
bool PoolAllocate(VideoState &s, BufferPool &pool, u64 budget, u64 chunk_bytes,
                  plume::RenderBufferFlags flags, const char *name, u64 bytes, u64 align,
                  plume::RenderBuffer **buffer, u64 *offset, u8 **cpu, bool *reset) {
  *reset = false;
  if (pool.totalBytes + bytes > budget) {
    for (auto &ch : pool.chunks) {
      pool.retired.push_back(std::move(ch.buffer));
      if (ch.vram)
        pool.retired.push_back(std::move(ch.vram));
    }
    pool.hasRetired.store(true, std::memory_order_release);
    pool.chunks.clear();
    pool.totalBytes = 0;
    *reset = true;
    EOT_DEBUG("[draw] {} pool over budget ({} MB); rebuilt", name, budget >> 20);
  }
  auto align_up = [align](u64 v) { return (v + align - 1) / align * align; };
  if (pool.chunks.empty() ||
      align_up(pool.chunks.back().used) + bytes > pool.chunks.back().capacity) {
    IndexCacheChunk ch;
    const u64 size = std::max(chunk_bytes, bytes + align);
    plume::RenderBufferDesc desc = plume::RenderBufferDesc::UploadBuffer(size);
    desc.flags = flags;
    ch.buffer = CreateHostBuffer(s.device.get(), desc, name);
    if (!ch.buffer)
      return false;
    ch.cpu = static_cast<u8 *>(ch.buffer->map());
    if (!ch.cpu) {
      ch.buffer.reset();
      return false;
    }
    ch.capacity = size;
    {
      plume::RenderBufferDesc vram_desc = plume::RenderBufferDesc::DefaultBuffer(size, flags);
      const std::string vram_name = std::string(name) + "-vram";
      ch.vram = CreateHostBuffer(s.device.get(), vram_desc, vram_name.c_str());
    }
    pool.chunks.push_back(std::move(ch));
  }
  auto &ch = pool.chunks.back();
  const u64 off = align_up(ch.used);
  *buffer = ch.buffer.get();
  *offset = off;
  *cpu = ch.cpu + off;
  ch.used = off + bytes;
  ch.accounted += bytes;
  pool.totalBytes += bytes;
  return true;
}

BufferPool &index_pool() {
  static BufferPool p;
  return p;
}

void PoolSeal(BufferPool &pool, plume::RenderBuffer *buffer, u64 end) {
  std::lock_guard lock(pool.mutex);
  for (auto &ch : pool.chunks) {
    if (ch.buffer.get() != buffer)
      continue;
    if (end > ch.sealed)
      ch.sealed = end;
    return;
  }
}

plume::RenderBuffer *PoolResidentBuffer(VideoState &s, BufferPool &pool,
                                        plume::RenderBuffer *upload, u64 offset, u64 bytes,
                                        u64 *capacity = nullptr) {
  std::lock_guard lock(pool.mutex);
  for (const auto &ch : pool.chunks) {
    if (ch.buffer.get() != upload)
      continue;
    if (capacity)
      *capacity = ch.capacity;
    if (ch.vram && offset + bytes <= ch.vramValidUpTo) {
      s.perf.geometry_vram_binds++;
      return ch.vram.get();
    }
    break;
  }
  s.perf.geometry_staging_binds++;
  return upload;
}

void PoolFlushToVram(VideoState &s, BufferPool &pool, plume::RenderBufferFlags flags) {
  auto *cmd = s.command_list;
  std::lock_guard lock(pool.mutex);
  for (auto &ch : pool.chunks) {
    if (!ch.vram || ch.sealed <= ch.vramValidUpTo)
      continue;
    const u64 from = ch.vramValidUpTo, bytes = ch.sealed - from;
    const plume::RenderBufferBarrier to_copy(ch.vram.get(), plume::RenderBufferAccess::WRITE);
    cmd->barriers(plume::RenderBarrierStage::COPY, &to_copy, 1, nullptr, 0);
    cmd->copyBufferRegion(plume::RenderBufferReference(ch.vram.get(), from),
                          plume::RenderBufferReference(ch.buffer.get(), from), bytes);
    const plume::RenderBufferBarrier to_read(ch.vram.get(), plume::RenderBufferAccess::READ);
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_read, 1, nullptr, 0);
    (void)flags;
    ch.vramValidUpTo = ch.sealed;
    pool.flushEpoch.fetch_add(1, std::memory_order_release);
  }
}
bool IndexCacheAllocate(VideoState &s, u64 bytes, plume::RenderBuffer **buffer, u64 *offset,
                        u8 **cpu) {
  auto &pool = index_pool();
  auto &cache = index_cache();
  if (bytes > kIndexCacheBudgetBytes)
    return false;
  std::lock_guard lock(pool.mutex);

  auto capacity = [&] {
    u64 c = 0;
    for (const auto &ch : pool.chunks)
      c += ch.capacity;
    return c;
  };
  auto needs_chunk = [&] {
    return pool.chunks.empty() || ((pool.chunks.back().used + 3) & ~3ull) + bytes > pool.chunks.back().capacity;
  };
  auto over_budget = [&] {
    return pool.totalBytes + bytes > kIndexCacheBudgetBytes ||
           (needs_chunk() && capacity() + std::max(kIndexCacheChunkBytes, bytes) > kIndexCacheBudgetBytes);
  };
  bool recycled_one = false;
  while (over_budget() && !pool.chunks.empty()) {
    auto victim = std::min_element(
        pool.chunks.begin(), pool.chunks.end(),
        [](const IndexCacheChunk &a, const IndexCacheChunk &b) {
          return a.lastUseFrame < b.lastUseFrame;
        });
    plume::RenderBuffer *dead = victim->buffer.get();
    u32 erased = 0;
    for (auto it = cache.map.begin(); it != cache.map.end();) {
      if (it->second.buffer != dead) {
        ++it;
        continue;
      }
      it = cache.map.erase(it);
      ++erased;
    }
    const u64 released = victim->accounted;
    const bool gpu_idle = victim->lastUseFrame + kNumFrames < s.guest_frames;
    IndexCacheChunk recycled;
    if (gpu_idle && bytes <= victim->capacity && !recycled_one) {
      recycled_one = true;
      recycled = std::move(*victim);
      recycled.used = 0;
      recycled.sealed = 0;
      recycled.accounted = 0;
      recycled.vramValidUpTo = 0;
      recycled.lastUseFrame = s.guest_frames;
    } else {
      pool.retired.push_back(std::move(victim->buffer));
      if (victim->vram)
        pool.retired.push_back(std::move(victim->vram));
      pool.hasRetired.store(true, std::memory_order_release);
    }
    pool.chunks.erase(victim);
    pool.totalBytes -= std::min(pool.totalBytes, released);
    if (recycled.buffer)
      pool.chunks.push_back(std::move(recycled));
    s.perf.index_cache_evictions++;
    u32 n;
    if (DiagShouldLog(0x5A11, &n))
      EOT_DEBUG("[draw] index-cache recycled one chunk ({} ranges, {} MB resident)", erased,
                pool.totalBytes >> 20);
  }
  bool reset = false;
  const bool ok = PoolAllocate(s, pool, kIndexCacheBudgetBytes, kIndexCacheChunkBytes,
                               plume::RenderBufferFlag::INDEX, "index-cache", bytes, 4, buffer,
                               offset, cpu, &reset);
  if (reset)
    cache.map.clear();
  if (ok) {
    for (auto &chunk : pool.chunks) {
      if (chunk.buffer.get() == *buffer) {
        chunk.lastUseFrame = s.guest_frames;
        break;
      }
    }
  }
  return ok;
}

void TouchIndexCacheBuffer(plume::RenderBuffer *buffer, u64 frame) {
  auto &pool = index_pool();
  std::lock_guard lock(pool.mutex);
  for (auto &chunk : pool.chunks) {
    if (chunk.buffer.get() == buffer) {
      chunk.lastUseFrame = frame;
      return;
    }
  }
}

struct VertexMirrorKey {
  u32 data_va = 0, size = 0;
  u64 seq = 0, sample = 0;
  bool operator==(const VertexMirrorKey &o) const {
    return data_va == o.data_va && size == o.size && seq == o.seq && sample == o.sample;
  }
};
struct VertexMirrorKeyHash {
  size_t operator()(const VertexMirrorKey &k) const {
    u64 h = 0x9E3779B97F4A7C15ull;
    auto mix = [&](u64 v) {
      h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      h *= 0xFF51AFD7ED558CCDull;
    };
    mix((u64(k.data_va) << 32) | k.size);
    mix(k.seq);
    mix(k.sample);
    return static_cast<size_t>(h);
  }
};
struct VertexMirror {
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u32 size = 0;
  u64 lastUseFrame = 0;
  mutable plume::RenderBuffer *resident = nullptr;
  mutable u64 residentCapacity = 0;
  mutable u64 residentEpoch = 0;
  u64 full = 0;  // WoS port (fix39): XXH3 of the guest range, for the stale-hit check
};
struct VertexMirrorCache {
  std::unordered_map<VertexMirrorKey, VertexMirror, VertexMirrorKeyHash> map;
  std::unordered_map<VertexMirrorKey, u64, VertexMirrorKeyHash> seen;
  BufferPool pool;
  bool admissionClosed = false;
};
VertexMirrorCache &vertex_mirrors() {
  static VertexMirrorCache c;
  return c;
}
constexpr u64 kVertexMirrorChunkBytes = 16ull << 20;
constexpr u64 kVertexMirrorBudgetBytes = 256ull << 20;
constexpr u32 kVertexMirrorMaxBytes = 32u << 20;
// WoS port (fix49): 4096 was below one city frame's distinct ranges (2-3k draws, several
// streams each): the set was cleared before a range's second sighting, so it was never admitted
// and its vertices were copied again every draw
constexpr size_t kVertexMirrorSeenCap = 65536;

// WoS port (fix99): most meshes are drawn more than once a frame (shadow cascades + main view);
// the second and later draws of a range in the same frame reuse the first lookup instead of
// re-sampling the guest bytes and searching the map. Invalidated with every map change.
struct VertexFrameMemo {
  u32 va = 0, size = 0;
  u64 seq = 0, frame = ~0ull, gen = 0;
  const VertexMirror *mirror = nullptr;
};
VertexFrameMemo g_vertex_memo[4096];
u64 g_vertex_memo_gen = 1;
extern "C" bool wos_vertex_frame_memo();

const VertexMirror *GetVertexMirrorSlow(VideoState &s, const StreamInfo &st, u64 first, u64 bytes);
const VertexMirror *GetVertexMirror(VideoState &s, const StreamInfo &st, u64 first, u64 bytes) {
  static const bool memo_on = wos_vertex_frame_memo();
  if (!memo_on || !st.dataVa || !bytes || bytes > kVertexMirrorMaxBytes || !st.data ||
      first > UINT32_MAX - st.dataVa)
    return GetVertexMirrorSlow(s, st, first, bytes);
  const u32 va = st.dataVa + static_cast<u32>(first), size = static_cast<u32>(bytes);
  const u64 seq = st.objectVa ? ResourceUnlockSeq(st.objectVa) : 0;
  VertexFrameMemo &m = g_vertex_memo[((va >> 4) ^ (size * 0x9E3779B1u) ^ static_cast<u32>(seq)) & 4095];
  if (m.mirror && m.va == va && m.size == size && m.seq == seq && m.frame == s.guest_frames &&
      m.gen == g_vertex_memo_gen) {
    s.perf.vertex_cache_hits++;
    return m.mirror;
  }
  const VertexMirror *r = GetVertexMirrorSlow(s, st, first, bytes);
  if (r) {
    m.va = va;
    m.size = size;
    m.seq = seq;
    m.frame = s.guest_frames;
    m.gen = g_vertex_memo_gen;
    m.mirror = r;
  }
  return r;
}

const VertexMirror *GetVertexMirrorSlow(VideoState &s, const StreamInfo &st, u64 first, u64 bytes) {
  if (!st.dataVa || !bytes || bytes > kVertexMirrorMaxBytes || !st.data ||
      first > UINT32_MAX - st.dataVa)
    return nullptr;
  auto &c = vertex_mirrors();
  VertexMirrorKey key;
  key.data_va = st.dataVa + static_cast<u32>(first);
  key.size = static_cast<u32>(bytes);
  key.seq = st.objectVa ? ResourceUnlockSeq(st.objectVa) : 0;
  key.sample = SampleHostBytes(st.data + first, key.size);
  auto it = c.map.find(key);
  if (it != c.map.end()) {
    it->second.lastUseFrame = s.guest_frames;
    s.perf.vertex_cache_hits++;
    // WoS port (fix39): every 64th hit, check the cached copy against the guest bytes - a
    // range rewritten under an unchanged fingerprint drew stale vertices
    static u64 hits = 0, checked = 0, stale = 0;
    // fix80 (wos_fast_capture): ranges over 64 KiB are checked 16x less often - the full hash
    // of a large range every 64th hit was a measurable share of the per-draw vertex cost
    static const bool fast = wos_fast_capture_on();
    ++hits;
    if ((hits & 63) == 0 && (!fast || key.size <= 65536u || (hits & 1023) == 0)) {
      ++checked;
      if (XXH3_64bits(st.data + first, key.size) != it->second.full) {
        ++stale;
        if (stale <= 24 || stale % 1000 == 0)
          EOT_WARN("[stale] vertex range {:#x}+{} (stride {}) changed under its fingerprint: "
                   "{} of {} checked hits stale; refreshed",
                   key.data_va, key.size, st.stride, stale, checked);
        c.map.erase(it);
        ++g_vertex_memo_gen;  // fix99
        return nullptr;
      }
    }
    return &it->second;
  }
  if (c.admissionClosed)
    return nullptr;
  auto seen = c.seen.find(key);
  if (seen == c.seen.end()) {
    if (c.seen.size() >= kVertexMirrorSeenCap)
      c.seen.clear();
    c.seen.emplace(key, s.guest_frames);
    return nullptr;
  }
  if (seen->second >= s.guest_frames)
    return nullptr;
  c.seen.erase(seen);
  if (c.pool.totalBytes > kVertexMirrorBudgetBytes ||
      bytes > kVertexMirrorBudgetBytes - c.pool.totalBytes) {
    c.admissionClosed = true;
    EOT_DEBUG("[draw] vertex-mirror cache full ({} MB, {} ranges); keeping residents and "
              "using frame uploads for new ranges",
              c.pool.totalBytes >> 20, c.map.size());
    return nullptr;
  }
  s.perf.vertex_cache_misses++;
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u8 *cpu = nullptr;
  bool reset = false;
  bool ok = false;
  {
    std::lock_guard lock(c.pool.mutex);
    ok = PoolAllocate(s, c.pool, kVertexMirrorBudgetBytes, kVertexMirrorChunkBytes,
                      plume::RenderBufferFlag::VERTEX, "vertex-mirror", bytes,
                      std::max<u64>(4, st.stride), &buffer, &offset, &cpu, &reset);
  }
  if (reset) {
    c.map.clear();
    ++g_vertex_memo_gen;  // fix99
  }
  if (!ok)
    return nullptr;
  {
    PerfScope copy_scope(s.perf.vertex_copy_ms);
    CopyVertexBytes(cpu, st.data + first, static_cast<u32>(bytes));
  }
  PoolSeal(c.pool, buffer, offset + bytes);
  s.perf.vertex_bytes += bytes;
  VertexMirror m;
  m.buffer = buffer;
  m.offset = offset;
  m.size = static_cast<u32>(bytes);
  m.lastUseFrame = s.guest_frames;
  m.full = XXH3_64bits(st.data + first, static_cast<size_t>(bytes));
  return &c.map.emplace(key, m).first->second;
}

void NoteMorphStream(VideoState &s, const StreamInfo &st, u64 first, u64 bytes) {
  struct Seen {
    u64 sample = 0, full = 0;
  };
  static std::unordered_map<u64, Seen> seen;
  static u64 frames = 0, last_frame = ~0ull, draws = 0, changed = 0, stale = 0;
  if (!st.data || !bytes || bytes > kVertexMirrorMaxBytes)
    return;
  const u64 key = (u64(st.dataVa + static_cast<u32>(first)) << 32) ^ bytes;
  const u64 sample = SampleHostBytes(st.data + first, static_cast<u32>(bytes));
  const u64 full = XXH3_64bits(st.data + first, static_cast<size_t>(bytes));
  auto [it, inserted] = seen.emplace(key, Seen{sample, full});
  if (!inserted) {
    if (it->second.full != full) {
      changed++;
      if (it->second.sample == sample)
        stale++;
    }
    it->second = Seen{sample, full};
  }
  draws++;
  if (last_frame != s.guest_frames) {
    last_frame = s.guest_frames;
    if (++frames % 600 == 0) {
      EOT_DEBUG("[morph] {} draws over {} frames: {} ranges, {} rewrites, {} of them under an "
                "unchanged fingerprint",
                draws, frames, seen.size(), changed, stale);
      if (seen.size() > 4096)
        seen.clear();
    }
  }
}

const CachedIndexRange *GetCachedIndexRange(VideoState &s, u32 ib_va, u32 prim, u32 start_index,
                                            u32 count) {
  if (!ib_va || !count || prim == kPrimRectList)
    return nullptr;
  const u8 *ib = mem::at<u8>(ib_va);
  if (!ib)
    return nullptr;
  const u32 common = rex::memory::load_and_swap<u32>(ib + obj::kCommon);
  const u32 data_va = rex::memory::load_and_swap<u32>(ib + obj::kBufferFetch0);
  const u32 size = rex::memory::load_and_swap<u32>(ib + obj::kBufferFetch1);
  if (!data_va)
    return nullptr;
  const bool is32 = (common & obj::kIndexBuffer32BitBit) != 0;
  const u32 elem = is32 ? 4 : 2;
  const u64 range_start = u64(start_index) * elem;
  u64 range_bytes = u64(count) * elem;
  if (size && range_start + range_bytes > size)
    range_bytes = range_start < size ? size - range_start : 0;
  if (!range_bytes)
    return nullptr;
  IndexCacheKey key;
  key.ib_va = ib_va;
  key.data_va = data_va;
  key.start = start_index;
  key.count = count;
  key.prim = prim;
  key.seq = ResourceUnlockSeq(ib_va);
  key.sample = SampleGuestBytes(data_va + static_cast<u32>(range_start), static_cast<u32>(range_bytes));

  auto &c = index_cache();
  auto it = c.map.find(key);
  if (it != c.map.end()) {
    // WoS port (fix39): the stale-hit check, as for vertex ranges
    static u64 hits = 0, checked = 0, stale = 0;
    bool fresh = true;
    static const bool fast = wos_fast_capture_on();
    ++hits;
    if ((hits & 63) == 0 && (!fast || range_bytes <= 65536u || (hits & 1023) == 0)) {
      ++checked;
      const u8 *guest = mem::gpu<u8>(data_va + static_cast<u32>(range_start));
      if (guest && XXH3_64bits(guest, static_cast<size_t>(range_bytes)) != it->second.full) {
        ++stale;
        fresh = false;
        if (stale <= 24 || stale % 1000 == 0)
          EOT_WARN("[stale] index range {:#x}+{} ({} indices) changed under its fingerprint: "
                   "{} of {} checked hits stale; refreshed",
                   data_va + static_cast<u32>(range_start), range_bytes, count, stale, checked);
        c.map.erase(it);
      }
    }
    if (fresh) {
      if (it->second.lastUseFrame != s.guest_frames) {
        it->second.lastUseFrame = s.guest_frames;
        TouchIndexCacheBuffer(it->second.buffer, s.guest_frames);
      }
      s.perf.index_cache_hits++;
      return &it->second;
    }
  }
  s.perf.index_cache_misses++;

  std::vector<u32> idx;
  if (!ReadGuestIndices(ib_va, start_index, count, idx))
    return nullptr;
  bool expand = false;
  plume::RenderPrimitiveTopology topology = ConvertPrimitiveType(prim, &expand);
  if (expand || prim == kPrimTriangleStrip || prim == kPrimLineStrip) {
    ExpandIndices(prim, idx, true);
    topology = prim == kPrimLineStrip ? plume::RenderPrimitiveTopology::LINE_LIST
                                      : plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  if (idx.empty())
    return nullptr;
  u32 lo = 0xFFFFFFFFu, hi = 0;
  for (u32 v : idx) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  const bool host32 = hi > 0xFFFEu;
  const u64 bytes = idx.size() * (host32 ? 4 : 2);
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u8 *cpu = nullptr;
  const bool allocated = s.ready && IndexCacheAllocate(s, bytes, &buffer, &offset, &cpu);
  PoolDrainRetired(s, index_pool());
  if (!allocated)
    return nullptr;
  if (host32) {
    std::memcpy(cpu, idx.data(), bytes);
  } else {
    auto *dst = reinterpret_cast<u16 *>(cpu);
    for (size_t i = 0; i < idx.size(); ++i)
      dst[i] = static_cast<u16>(idx[i]);
  }
  PoolSeal(index_pool(), buffer, offset + bytes);
  CachedIndexRange r;
  r.buffer = buffer;
  r.offset = offset;
  r.count = static_cast<u32>(idx.size());
  r.lo = lo;
  r.hi = hi;
  r.is32 = host32;
  r.topology = topology;
  r.lastUseFrame = s.guest_frames;
  if (const u8 *guest = mem::gpu<u8>(data_va + static_cast<u32>(range_start)))
    r.full = XXH3_64bits(guest, static_cast<size_t>(range_bytes));
  return &(c.map[key] = r);
}

struct RectExpansion {
  std::vector<u8> vertices[16];
  std::vector<u32> indices;
  u32 hostVertexCount = 0;
};

float LaneToFloat(const u8 *p, plume::RenderFormat f, u32 lane, bool *ok) {
  using F = plume::RenderFormat;
  *ok = true;
  switch (f) {
  case F::R32_FLOAT:
  case F::R32G32_FLOAT:
  case F::R32G32B32_FLOAT:
  case F::R32G32B32A32_FLOAT: {
    float v;
    std::memcpy(&v, p + lane * 4, 4);
    return v;
  }
  case F::R16G16_FLOAT:
  case F::R16G16B16A16_FLOAT:
  case F::R16_FLOAT: {
    u16 h;
    std::memcpy(&h, p + lane * 2, 2);
    const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    float v;
    if (exp == 0)
      v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31)
      v = man ? NAN : INFINITY;
    else
      v = std::ldexp(static_cast<float>(man | 0x400), static_cast<int>(exp) - 25);
    return sign ? -v : v;
  }
  default:
    *ok = false;
    return 0.0f;
  }
}

void SynthesizeLane(u8 *dst, const u8 *vi, const u8 *vj, const u8 *vk, plume::RenderFormat f,
                    u32 bytes) {
  using F = plume::RenderFormat;
  auto lanes_of = [](F fmt, u32 *lane_bytes) -> u32 {
    switch (fmt) {
    case F::R32_FLOAT:
      *lane_bytes = 4;
      return 1;
    case F::R32G32_FLOAT:
      *lane_bytes = 4;
      return 2;
    case F::R32G32B32_FLOAT:
      *lane_bytes = 4;
      return 3;
    case F::R32G32B32A32_FLOAT:
      *lane_bytes = 4;
      return 4;
    case F::R16_FLOAT:
    case F::R16_UNORM:
    case F::R16_SNORM:
      *lane_bytes = 2;
      return 1;
    case F::R16G16_FLOAT:
    case F::R16G16_UNORM:
    case F::R16G16_SNORM:
    case F::R16G16_UINT:
      *lane_bytes = 2;
      return 2;
    case F::R16G16B16A16_FLOAT:
    case F::R16G16B16A16_UNORM:
    case F::R16G16B16A16_SNORM:
    case F::R16G16B16A16_UINT:
      *lane_bytes = 2;
      return 4;
    case F::R8G8B8A8_UNORM:
    case F::B8G8R8A8_UNORM:
    case F::R8G8B8A8_SNORM:
    case F::R8G8B8A8_UINT:
      *lane_bytes = 1;
      return 4;
    case F::R8G8_UNORM:
    case F::R8G8_SNORM:
    case F::R8G8_UINT:
      *lane_bytes = 1;
      return 2;
    case F::R8_UNORM:
    case F::R8_SNORM:
    case F::R8_UINT:
      *lane_bytes = 1;
      return 1;
    default:
      *lane_bytes = 0;
      return 0;
    }
  };
  u32 lane_bytes = 0;
  const u32 lanes = lanes_of(f, &lane_bytes);
  if (!lanes) {
    std::memcpy(dst, vi, bytes);
    return;
  }
  for (u32 l = 0; l < lanes; ++l) {
    bool ok = false;
    if (lane_bytes == 4 || (lane_bytes == 2 && (f == F::R16_FLOAT || f == F::R16G16_FLOAT ||
                                                 f == F::R16G16B16A16_FLOAT))) {
      const float a = LaneToFloat(vi, f, l, &ok), b = LaneToFloat(vj, f, l, &ok),
                  c = LaneToFloat(vk, f, l, &ok);
      const float r = a + b - c;
      if (lane_bytes == 4) {
        std::memcpy(dst + l * 4, &r, 4);
      } else {
        std::memcpy(dst + l * 2, vi + l * 2, 2);
      }
    } else if (lane_bytes == 2) {
      u16 a, b, c;
      std::memcpy(&a, vi + l * 2, 2);
      std::memcpy(&b, vj + l * 2, 2);
      std::memcpy(&c, vk + l * 2, 2);
      const i32 r = std::clamp(static_cast<i32>(a) + b - c, 0, 65535);
      const u16 rv = static_cast<u16>(r);
      std::memcpy(dst + l * 2, &rv, 2);
    } else {
      const i32 r = std::clamp(static_cast<i32>(vi[l]) + vj[l] - vk[l], 0, 255);
      dst[l] = static_cast<u8>(r);
    }
  }
}

bool ExpandRectList(const InputLayout &layout, const StreamInfo streams[16], u32 stream_mask,
                    const std::vector<u32> &guest_vertices, RectExpansion &out) {
  const plume::RenderInputElement *pos = nullptr;
  for (const auto &e : layout.elements) {
    if (std::strcmp(e.semanticName, "POSITION") == 0 && (e.semanticIndex & 0xFF) == 0 &&
        e.slotIndex != kSyntheticVertexSlot) {
      pos = &e;
      break;
    }
  }
  const u32 rects = static_cast<u32>(guest_vertices.size() / 3);
  out.hostVertexCount = rects * 4;
  out.indices.reserve(rects * 6);
  for (u32 S = 0; S < 16; ++S) {
    if (stream_mask & (1u << S))
      out.vertices[S].resize(size_t(out.hostVertexCount) * streams[S].stride);
  }
  for (u32 r = 0; r < rects; ++r) {
    const u32 g[3] = {guest_vertices[r * 3], guest_vertices[r * 3 + 1],
                      guest_vertices[r * 3 + 2]};
    u32 k = 0;
    if (pos) {
      const StreamInfo &ps = streams[pos->slotIndex];
      float px[3], py[3];
      bool ok = true;
      for (u32 n = 0; n < 3 && ok; ++n) {
        const u8 *v = ps.data + u64(g[n]) * ps.stride + pos->alignedByteOffset;
        if ((u64(g[n]) + 1) * ps.stride > ps.sizeBytes && ps.sizeBytes) {
          ok = false;
          break;
        }
        u8 tmp[16];
        CopyVertexBytes(tmp, v, 16);
        px[n] = LaneToFloat(tmp, pos->format, 0, &ok);
        py[n] = LaneToFloat(tmp, pos->format, 1, &ok);
      }
      if (ok) {
        float best = INFINITY;
        for (u32 c = 0; c < 3; ++c) {
          const u32 i = (c + 1) % 3, j = (c + 2) % 3;
          const float ax = px[i] - px[c], ay = py[i] - py[c];
          const float bx = px[j] - px[c], by = py[j] - py[c];
          const float dot = std::fabs(ax * bx + ay * by);
          const float norm = std::sqrt((ax * ax + ay * ay) * (bx * bx + by * by)) + 1e-20f;
          const float score = dot / norm;
          if (score < best) {
            best = score;
            k = c;
          }
        }
      }
    }
    const u32 i = (k + 1) % 3, j = (k + 2) % 3;
    const u32 base = r * 4;
    for (u32 S = 0; S < 16; ++S) {
      if (!(stream_mask & (1u << S)))
        continue;
      const StreamInfo &st = streams[S];
      u8 *dst = out.vertices[S].data() + size_t(base) * st.stride;
      for (u32 n = 0; n < 3; ++n) {
        const u8 *src = st.data + u64(g[n]) * st.stride;
        CopyVertexBytes(dst + n * st.stride, src, st.stride);
      }
      u8 *v3 = dst + 3 * st.stride;
      std::memcpy(v3, dst + i * st.stride, st.stride);
      for (const auto &e : layout.elements) {
        if (e.slotIndex != S)
          continue;
        const u32 bytes = std::min(16u, st.stride - e.alignedByteOffset);
        SynthesizeLane(v3 + e.alignedByteOffset, dst + i * st.stride + e.alignedByteOffset,
                       dst + j * st.stride + e.alignedByteOffset,
                       dst + k * st.stride + e.alignedByteOffset, e.format, bytes);
      }
    }
    out.indices.push_back(base + 0);
    out.indices.push_back(base + 1);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + i);
    out.indices.push_back(base + 3);
    out.indices.push_back(base + j);
  }
  return true;
}

void FillPipelineState(DeviceView dev, const Targets &t, PipelineState &st,
                       u32 *spec_out, bool *alpha_to_coverage_only) {
  const u32 mode = dev.U32(dev::kModeControl);
  const bool cull_front = mode & 1, cull_back = mode & 2, face_cw = mode & 4;
  if (cull_front && cull_back)
    st.cull = plume::RenderCullMode::BACK;
  else if (cull_front)
    st.cull = plume::RenderCullMode::FRONT;
  else if (cull_back)
    st.cull = plume::RenderCullMode::BACK;
  else
    st.cull = plume::RenderCullMode::NONE;
  if (eot::debug::FreecamDrawTwoSided())
    st.cull = plume::RenderCullMode::NONE;
  // WoS port (fix86): diagnostics for the missing / black ground patches
  static const bool no_cull = wos_debug_no_cull();
  if (no_cull)
    st.cull = plume::RenderCullMode::NONE;
  st.frontFace = face_cw ? plume::RenderFrontFace::CLOCKWISE
                         : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  const u32 clip = dev.U32(dev::kClipControl);
  st.depthClip = (clip & ((1u << 16) | (1u << 26) | (1u << 27))) == 0;
  // WoS port (fix87): wos_debug_depth_clamp - clamp depth instead of clipping (a draw whose
  // z lands outside [0,1] shows up instead of vanishing)
  static const bool depth_clamp = wos_debug_depth_clamp();
  if (depth_clamp)
    st.depthClip = false;
  if (!st.depthClip) {
    u32 n;
    if (DiagShouldLog(0x6C1F, &n) && n < 3)
      EOT_DEBUG("[draw] clip control {:#x}: depth clip off", clip);
  }

  if (mode & (1u << 11)) {
    const float scale = dev.F32(dev::kPolyOffsetFrontScale);
    const float offset = dev.F32(dev::kPolyOffsetFrontOffset);
    st.slopeScaledDepthBias = scale / 16.0f;
    st.depthBias = PolygonOffsetUnits(offset);
  }
  const float rs = RenderScaleFactor();
  const float ts = std::fabs(t.scale - rs) < 0.01f ? rs : t.scale;
  st.targetScale = (st.depthBias || st.slopeScaledDepthBias != 0.0f) ? ts : 1.0f;

  const u32 dc = dev.U32(dev::kDepthControl);
  const bool has_ds = t.depth != nullptr;
  const bool z_test = has_ds && (dc & 2);
  st.depthWrite = has_ds && (dc & 4);
  st.depthEnable = z_test || st.depthWrite;
  st.depthFunc = z_test ? ConvertCompareFunc((dc >> 4) & 7)
                        : plume::RenderComparisonFunction::ALWAYS;
  st.stencilEnable = has_ds && (dc & 1);
  // WoS port (fix86): diagnostics - colour draws ignore the depth test / every draw ignores
  // the stencil test
  static const bool depth_always = wos_debug_depth_always();
  static const bool no_stencil = wos_debug_no_stencil();
  if (depth_always && t.colorCount)
    st.depthFunc = plume::RenderComparisonFunction::ALWAYS;
  if (no_stencil)
    st.stencilEnable = false;
  if (st.stencilEnable && !FormatHasStencil(t.depthImage ? t.depthImage->format : t.depth->host.format)) {
    u32 n;
    if (DiagShouldLog(0x6C30, &n) && n == 0)
      EOT_WARN("[draw] a draw into the stencil-less depth target {:#x} ({}x{}) enables the stencil test "
               "(control {:#x}); drawn without it",
               t.depth->va, t.depth->width, t.depth->height, dc);
    st.stencilEnable = false;
  }
  if (st.stencilEnable) {
    const u32 srm = dev.U32(dev::kStencilRefMask);
    st.stencilReadMask = static_cast<u8>((srm >> 8) & 0xFF);
    st.stencilWriteMask = static_cast<u8>((srm >> 16) & 0xFF);
    st.stencilRef = static_cast<u8>(srm & 0xFF);
    st.stencilFront.compareFunction = ConvertCompareFunc((dc >> 8) & 7);
    st.stencilFront.failOp = ConvertStencilOp((dc >> 11) & 7);
    st.stencilFront.passOp = ConvertStencilOp((dc >> 14) & 7);
    st.stencilFront.depthFailOp = ConvertStencilOp((dc >> 17) & 7);
    if (dc & (1u << 7)) {
      st.stencilBack.compareFunction = ConvertCompareFunc((dc >> 20) & 7);
      st.stencilBack.failOp = ConvertStencilOp((dc >> 23) & 7);
      st.stencilBack.passOp = ConvertStencilOp((dc >> 26) & 7);
      st.stencilBack.depthFailOp = ConvertStencilOp((dc >> 29) & 7);
    } else {
      st.stencilBack = st.stencilFront;
    }
  } else {
    st.stencilFront.compareFunction = plume::RenderComparisonFunction::ALWAYS;
    st.stencilBack.compareFunction = plume::RenderComparisonFunction::ALWAYS;
  }

  const u32 cc = dev.U32(dev::kColorControl);
  const bool blend_disable = (cc & (1u << 5)) != 0;
  const u32 color_mask = dev.U32(dev::kColorMask);
  static const u32 kBlendRegs[4] = {dev::kBlendControl0, dev::kBlendControl1,
                                    dev::kBlendControl2, dev::kBlendControl3};
  for (u32 i = 0; i < t.colorCount; ++i) {
    const u32 bc = dev.U32(kBlendRegs[i]);
    const u32 src = bc & 0x1F, op = (bc >> 5) & 7, dst = (bc >> 8) & 0x1F;
    const u32 srca = (bc >> 16) & 0x1F, opa = (bc >> 21) & 7, dsta = (bc >> 24) & 0x1F;
    const bool passthrough = src == 1 && dst == 0 && op == 0 && srca == 1 && dsta == 0 && opa == 0;
    plume::RenderBlendDesc &b = st.blend[i];
    b.blendEnabled = !blend_disable && !passthrough;
    b.srcBlend = ConvertBlendFactor(src);
    b.dstBlend = ConvertBlendFactor(dst);
    b.blendOp = ConvertBlendOp(op);
    b.srcBlendAlpha = ConvertBlendFactor(srca);
    b.dstBlendAlpha = ConvertBlendFactor(dsta);
    b.blendOpAlpha = ConvertBlendOp(opa);
    b.renderTargetWriteMask = static_cast<u8>((color_mask >> (4 * i)) & 0xF);
    st.rtFormats[i] = t.colorImage[i] ? t.colorImage[i]->format : t.color[i]->host.format;
  }
  st.rtCount = t.colorCount;
  st.dsFormat = has_ds ? (t.depthImage ? t.depthImage->format : t.depth->host.format)
                       : plume::RenderFormat::UNKNOWN;
  st.sampleCount = t.samples;
  st.alphaToCoverage = (cc & (1u << 4)) != 0;
  *alpha_to_coverage_only = st.alphaToCoverage;

  u32 spec = 0;
  if (cc & (1u << 3)) {
    const u32 func = cc & 7;
    if (func == 4 || func == 6) {
      spec |= kSpecAlphaTest;
    } else if (func == 0) {
      for (u32 i = 0; i < t.colorCount; ++i)
        st.blend[i].renderTargetWriteMask = 0;
    } else if (func != 7) {
      u32 n;
      if (DiagShouldLog(0x6200 + func, &n))
        EOT_WARN("[draw] alpha test function {} not modelled (treated as pass)", func);
    }
  }
  *spec_out = spec;
}

struct SamplerBindings {
  SharedConstants shared;
};

bool RefreshResolvedMirror(VideoState &s, GuestTexture &mirror, const Targets &t) {
  if (!mirror.resolveOwned || mirror.host.isDepth || !mirror.lastResolve)
    return false;
  GuestSurface *surf = nullptr;
  for (u32 i = 0; i < t.colorCount; ++i) {
    if (t.color[i] && t.color[i]->uid == mirror.resolvedSurfaceUid) {
      surf = t.color[i];
      break;
    }
  }
  if (!surf || surf->serial == mirror.resolvedSurfaceSerial ||
      mirror.resolvedSurfaceSerial < surf->wholeClearSerial)
    return false;
  ResolvePacket pk = *mirror.lastResolve;
  pk.refresh = true;
  pk.flags &= ~0x300u;
  const bool defer = s.defer_shader_read_transitions;
  s.defer_shader_read_transitions = false;
  ReplayResolveLocked(s, pk);
  s.defer_shader_read_transitions = defer;
  return s.command_list_open;
}

bool FlushAliasCopyForDraw(VideoState &s, GuestTexture &t) {
  const bool defer = s.defer_shader_read_transitions;
  s.defer_shader_read_transitions = false;
  const bool copied = FlushAliasCopy(s, t);
  s.defer_shader_read_transitions = defer;
  return copied;
}

void BindTexturesAndSamplers(VideoState &s, DeviceView dev, u32 texture_mask,
                             SharedConstants &sc, const Targets *refresh_targets) {
  const u32 sampler_policy = static_cast<u32>(Settings::Anisotropy());
  const bool refresh_copies = refresh_targets && Settings::SceneCopyRefresh();
  struct DeferGuard {
    VideoState &s;
    explicit DeferGuard(VideoState &state) : s(state) { s.defer_shader_read_transitions = true; }
    ~DeferGuard() { s.defer_shader_read_transitions = false; }
  } defer_guard(s);
  for (u32 i = 0; i < 16; ++i) {
    sc.texture2DIndices[i] = kNullTexture2DDescriptorIndex;
    sc.texture3DIndices[i] = kNullTexture3DDescriptorIndex;
    sc.textureCubeIndices[i] = kNullTextureCubeDescriptorIndex;
    sc.texture1DIndices[i] = kNullTexture2DDescriptorIndex;
    sc.samplerIndices[i] = kSamplerLinearClamp;
  }
  const u64 generation = s.texture_generation.load(std::memory_order_relaxed);
  for (u32 slot = 0; slot < 16; ++slot)
    s.draw_bound_textures[slot] = nullptr;
  bool copied_while_binding = false;
  for (u32 slot = 0; slot < 16; ++slot) {
    if (!(texture_mask & (1u << slot)))
      continue;
    u32 tex_va = dev.U32(dev::kTextureObject0 + 4 * slot);
    const u8 *fc_raw = dev.Bytes(dev::kFetchConstants + 24 * slot, 24);
    if (!fc_raw || (fc_raw[3] & 3) != 2)
      continue;
    // WoS port (fix68): diagnostic - wos_debug_no_shadow_maps leaves every depth-format texture
    // (k_24_8 / k_24_8_FLOAT: the shadow maps) unbound, i.e. nothing is in shadow
    {
      static const bool no_shadow_maps = wos_debug_no_shadow_maps();
      const u32 fmt = (u32(fc_raw[4]) << 24 | u32(fc_raw[5]) << 16 | u32(fc_raw[6]) << 8 | fc_raw[7]) & 0x3F;
      if (no_shadow_maps && (fmt == 22 || fmt == 23))
        continue;
      static const u32 null_slots = static_cast<u32>(wos_debug_null_slots());
      if (null_slots & (1u << slot)) {
        // only in draws that bind a cube map somewhere (fetch dword5 dimension 3)
        bool cube = false;
        for (u32 c = 0; c < 16 && !cube; ++c) {
          const u8 *f = dev.Bytes(dev::kFetchConstants + 24 * c, 24);
          cube = f && (f[3] & 3) == 2 && ((f[22] >> 1) & 3) == 3;
        }
        if (cube)
          continue;
      }
    }
    // WoS port: textures set as bare fetch constants (no texture object - the world's
    // are written straight into the command stream) are keyed by their storage: the
    // physical base page in the E0 view plus the format, unique per texture.
    if (!tex_va) {
      const u32 w1 = rex::memory::load_and_swap<u32>(fc_raw + 4);
      if (!(w1 & 0xFFFFF000u))
        continue;
      // (keyed by the physical address: a virtual E0-view base sits 0x1000 above it)
      u32 base = w1 & 0xFFFFF000u;
      if (base >= 0x20000000u)
        base = (base & 0x1FFFFFFFu) + (base >= 0xE0000000u ? 0x1000u : 0u);
      tex_va = 0xE0000000u | (base & 0x1FFFF000u) | ((w1 & 0x3Fu) << 4);
      // WoS port (fix34): and a hash of its size, mip address and dimension in the free bits
      // (0-3, 10-11) - two textures over the same storage with other sizes (the world's
      // atlases and mip views) no longer recreate each other every draw
      {
        const u32 w2 = rex::memory::load_and_swap<u32>(fc_raw + 8);
        const u32 w5 = rex::memory::load_and_swap<u32>(fc_raw + 20);
        u32 h = (w2 * 0x9E3779B1u) ^ ((w5 & 0xFFFFF600u) * 0x85EBCA77u);
        h ^= h >> 15;
        tex_va |= (h & 0xFu) | (((h >> 4) & 3u) << 10);
      }
    }
    s.perf.texture_bind_requests++;
    GuestTexture *gt = nullptr;
    u32 index = kInvalidDescriptorIndex;
    u32 sampler = 0;
    u8 dimension = 0, biased_bits = 0;
    VideoState::TextureSlotCache *hit = nullptr;
    for (u32 w = 0; w < VideoState::kTextureSlotWays; ++w) {
      VideoState::TextureSlotCache &cs = s.slot_cache[slot][w];
      if (cs.texVa == tex_va && cs.generation == generation && cs.texture && cs.texture->aliasPending)
        copied_while_binding |= FlushAliasCopyForDraw(s, *cs.texture);
      if (cs.texVa == tex_va && cs.generation == generation && cs.texture &&
          cs.resourceGeneration == cs.texture->bindingGeneration &&
          cs.samplerPolicy == sampler_policy && std::memcmp(cs.fc, fc_raw, sizeof(cs.fc)) == 0 &&
          // WoS port: a CPU-written texture's bytes are compared once a frame (Bink video)
          !(cs.texture->contentHash && cs.texture->contentCheckFrame != s.guest_frames)) {
        hit = &cs;
        break;
      }
    }
    if (hit) {
      gt = hit->texture;
      gt->lastUseFrame = s.guest_frames;
      gt->lastSampledFrame = s.guest_frames;
      if (refresh_copies && gt->lastResolve)
        copied_while_binding |= RefreshResolvedMirror(s, *gt, *refresh_targets);
      TransitionLocked(s, gt->host, plume::RenderTextureLayout::SHADER_READ);
      index = hit->index;
      sampler = hit->sampler;
      dimension = hit->dimension;
      biased_bits = hit->biasedBits;
      s.perf.texture_bind_hits++;
    } else {
      u32 fc[6];
      for (u32 d = 0; d < 6; ++d)
        fc[d] = rex::memory::load_and_swap<u32>(fc_raw + 4 * d);
      gt = GetGuestTexture(s, tex_va, true, fc);  // WoS port: stack headers
      if (!gt) {
        u32 n;
        if (DiagShouldLog(0x6300 ^ tex_va, &n))
          EOT_WARN("[draw] slot {} texture {:#x} has no host mirror (fetch fmt {}); sampling null",
                   slot, tex_va, (fc[1] >> 0) & 0x3F);
        continue;
      }
      const u32 swizzle = (fc[3] >> 1) & 0xFFF;
      if (gt->aliasPending)
        copied_while_binding |= FlushAliasCopyForDraw(s, *gt);
      gt->lastSampledFrame = s.guest_frames;
      if (refresh_copies && gt->lastResolve)
        copied_while_binding |= RefreshResolvedMirror(s, *gt, *refresh_targets);
      index = PrepareTextureForSampling(s, *gt, swizzle);
      if (index == kInvalidDescriptorIndex) {
        u32 n;
        if (DiagShouldLog(0x6380 ^ tex_va, &n))
          EOT_WARN("[draw] slot {} texture {:#x} (fmt {} {}x{} {}) could not be bound; sampling null",
                   slot, tex_va, static_cast<u32>(gt->format), gt->width, gt->height,
                   gt->host.isDepth ? "depth" : "colour");
        continue;
      }
      sampler = ResolveSamplerSlotLocked(DecodeSamplerFromFetch(
          fc, !gt->resolveOwned && !gt->host.isDepth && gt->host.mipLevels > 1, gt->synthMips));
      dimension = static_cast<u8>(gt->dimension);
      const u32 sign_x = (fc[0] >> 2) & 3;
      const u32 sign_w = (fc[0] >> 8) & 3;
      if (sign_x == 2)
        biased_bits |= 1;
      if (sign_x == 3) {
        const bool srgb_view = gt->host.format == plume::RenderFormat::BC1_UNORM_SRGB ||
                               gt->host.format == plume::RenderFormat::BC2_UNORM_SRGB ||
                               gt->host.format == plume::RenderFormat::BC3_UNORM_SRGB;
        if (!srgb_view)
          biased_bits |= 2;
      }
      if (sign_w == 2)
        biased_bits |= 4;
      u8 &next = s.slot_cache_next[slot];
      VideoState::TextureSlotCache &cs = s.slot_cache[slot][next];
      next = static_cast<u8>((next + 1) % VideoState::kTextureSlotWays);
      cs.texVa = tex_va;
      std::memcpy(cs.fc, fc_raw, sizeof(cs.fc));
      cs.generation = s.texture_generation.load(std::memory_order_relaxed);
      cs.resourceGeneration = gt->bindingGeneration;
      cs.samplerPolicy = sampler_policy;
      cs.texture = gt;
      cs.index = index;
      cs.sampler = sampler;
      cs.dimension = dimension;
      cs.biasedBits = biased_bits;
    }
    gt->perfSamples++;
    s.draw_bound_textures[slot] = gt;
    // WoS port (fix101): the black square. The street there samples its horizon map
    // (16_16_16_16_EXPAND 512x256) from 0x1CAF1000, scratch memory the game reuses every
    // frame: the shadow map (24_8_FLOAT 1024x1024) is resolved there before the street is
    // drawn, the bloom (same 512x256 format) after it. On the console / emulator the street
    // reads the shadow map's bytes (bright: the street lit); the host kept the two as separate
    // textures and handed it LAST frame's bloom - black except the glow of a projectile - and
    // a horizon of 0 is an unlit street. A resolve target whose memory a later resolve of
    // another format overwrote is sampled as opaque white (full sky) instead.
    if (gt->resolveOwned && s.white_texture_index != kInvalidDescriptorIndex &&
        dimension != static_cast<u8>(xe::DataDimension::k3D) &&
        dimension != static_cast<u8>(xe::DataDimension::kCube)) {
      static const bool white_stale = wos_stale_alias_white();
      if (white_stale) {
        auto it = s.resolve_writes.find(gt->baseAddress);
        if (it != s.resolve_writes.end() && it->second.first > gt->resolveWriteSeq &&
            it->second.second != static_cast<u32>(gt->format)) {
          index = s.white_texture_index;
          s.perf.stale_alias_white++;
          u32 n;
          if (DiagShouldLog(0x7A00 ^ gt->baseAddress, &n) && n < 4)
            EOT_INFO("[draw] fix101: slot {} reads {:#x} fmt {} {}x{}, overwritten since by a fmt {} "
                     "resolve: sampled as white",
                     slot, gt->baseAddress, static_cast<u32>(gt->format), gt->width, gt->height,
                     it->second.second);
        }
      }
    }
    switch (static_cast<xe::DataDimension>(dimension)) {
    case xe::DataDimension::k3D:
      sc.texture3DIndices[slot] = index;
      break;
    case xe::DataDimension::kCube:
      sc.textureCubeIndices[slot] = index;
      break;
    default:
      sc.texture2DIndices[slot] = index;
      sc.texture1DIndices[slot] = index;
      break;
    }
    sc.samplerIndices[slot] = sampler;
    if (biased_bits & 1)
      sc.biasedTextures |= 1u << slot;
    if (biased_bits & 2)
      sc.biasedTextures |= 1u << (16 + slot);
    if (biased_bits & 4)
      sc.sintTexcoords |= 1u << (16 + slot);
  }
  if (copied_while_binding)
    for (u32 slot = 0; slot < 16; ++slot)
      if (GuestTexture *bound = s.draw_bound_textures[slot])
        TransitionLocked(s, bound->host, plume::RenderTextureLayout::SHADER_READ);
}

bool UploadZeroBuffer(VideoState &s, UploadAlloc *out) {
  static UploadAlloc cached[kNumFrames];
  static u64 cached_frame[kNumFrames] = {};
  static bool cached_valid[kNumFrames] = {};
  const u32 slot = s.recording_slot();
  if (cached_valid[slot] && cached_frame[slot] == UploadRingEpoch()) {
    *out = cached[slot];
    return true;
  }
  if (!UploadAllocate(4096, 16, out))
    return false;
  std::memset(out->cpu, 0, 4096);
  cached[slot] = *out;
  cached_frame[slot] = UploadRingEpoch();
  cached_valid[slot] = true;
  return true;
}

static_assert(dev::kPsFloatConstants + 256u * 16u == dev::kVsBoolConstants);

bool VelocityCandidate(DeviceView dev, const TargetWords &tw) {
  if (tw.colorCount != 1 || !tw.depthVa || !tw.colorVa[0])
    return false;
  if (!(dev.U32(dev::kDepthControl) & 4))
    return false;
  if ((dev.U32(dev::kVteControl) & 5) != 5)
    return false;
  const float xs = std::fabs(dev.F32(dev::kVportXScale)), ys = std::fabs(dev.F32(dev::kVportYScale));
  if (std::fabs(xs * 2.0f - static_cast<float>(kGuestRenderWidth)) >= 1.0f ||
      std::fabs(ys * 2.0f - static_cast<float>(kGuestRenderHeight)) >= 1.0f)
    return false;
  const u32 *w = tw.colorWords[0];
  const u32 width = (w[3] >> 18) + 1, height = ((w[3] >> 3) & 0x7FFF) + 1;
  if (width != kGuestRenderWidth || height != kGuestRenderHeight)
    return false;
  return ((w[1] >> 16) & 0xF) == 7; // ColorRenderTargetFormat k_16_16_16_16_FLOAT
}

bool ShadowTileWords(const u32 words[5]) {
  const u32 size_bits = words[3];
  return (size_bits >> 18) + 1 == 1024 && ((size_bits >> 3) & 0x7FFF) + 1 == 1024;
}

struct ShadowPassCensus {
  u64 frame = ~0ull;
  u32 pass = 0;
  u64 hash = 0;
  u32 casters = 0;
  u32 skinned = 0;
  u64 staticHash = 0;
  u64 lastHashes[64] = {};
  u64 lastStaticHashes[64] = {};
  u32 lastCasters[64] = {};
  u32 lastPasses = 0;
  u64 hashes[64] = {};
  u64 staticHashes[64] = {};
  u32 castersOf[64] = {};
  u64 sumPasses = 0, sumRepeats = 0, sumCasters = 0, sumRepeatCasters = 0, frames = 0;
  u64 sumSkinned = 0, sumStaticRepeatPasses = 0, sumStaticRepeatCasters = 0;
  u64 lastPrint = 0;
};
ShadowPassCensus g_shadow_census;

void ShadowPassClose(ShadowPassCensus &c) {
  if (!c.casters)
    return;
  if (c.pass < 64) {
    c.hashes[c.pass] = c.hash;
    c.staticHashes[c.pass] = c.staticHash;
    c.castersOf[c.pass] = c.casters;
    const bool repeat = c.skinned == 0 && c.pass < c.lastPasses && c.lastHashes[c.pass] == c.hash;
    const bool static_repeat = c.pass < c.lastPasses && c.lastStaticHashes[c.pass] == c.staticHash;
    c.sumPasses++;
    c.sumCasters += c.casters;
    c.sumSkinned += c.skinned;
    if (repeat) {
      c.sumRepeats++;
      c.sumRepeatCasters += c.casters;
    }
    if (static_repeat) {
      c.sumStaticRepeatPasses++;
      c.sumStaticRepeatCasters += c.casters - c.skinned;
    }
  }
  c.pass++;
  c.hash = 0;
  c.staticHash = 0;
  c.casters = 0;
  c.skinned = 0;
}

void NoteShadowCaster(VideoState &s, DeviceView dev, const GuestShader &vs, const StreamInfo streams[16],
                      u32 lo, u32 hi) {
  if (Settings::DiagVerbosity() < 2)
    return;
  ShadowPassCensus &c = g_shadow_census;
  if (c.frame != s.guest_frames) {
    ShadowPassClose(c);
    std::memcpy(c.lastHashes, c.hashes, sizeof(c.hashes));
    std::memcpy(c.lastStaticHashes, c.staticHashes, sizeof(c.staticHashes));
    std::memcpy(c.lastCasters, c.castersOf, sizeof(c.castersOf));
    c.lastPasses = std::min(c.pass, 64u);
    c.pass = 0;
    c.frames++;
    c.frame = s.guest_frames;
    if (s.guest_frames - c.lastPrint >= 300) {
      c.lastPrint = s.guest_frames;
      if (c.frames) {
        EOT_DEBUG("[shadow] census over {} frames: {:.1f} passes a frame, {:.1f} whole repeats of the previous "
                  "frame ({:.0f}%), {:.1f} static repeats ({:.0f}%); {:.0f} casters a frame, {:.0f} skinned, "
                  "{:.0f} in whole repeats, {:.0f} static casters in static repeats",
                  c.frames, static_cast<f64>(c.sumPasses) / c.frames,
                  static_cast<f64>(c.sumRepeats) / c.frames,
                  c.sumPasses ? 100.0 * c.sumRepeats / c.sumPasses : 0.0,
                  static_cast<f64>(c.sumStaticRepeatPasses) / c.frames,
                  c.sumPasses ? 100.0 * c.sumStaticRepeatPasses / c.sumPasses : 0.0,
                  static_cast<f64>(c.sumCasters) / c.frames, static_cast<f64>(c.sumSkinned) / c.frames,
                  static_cast<f64>(c.sumRepeatCasters) / c.frames,
                  static_cast<f64>(c.sumStaticRepeatCasters) / c.frames);
      }
      c.sumPasses = c.sumRepeats = c.sumCasters = c.sumRepeatCasters = c.frames = 0;
      c.sumSkinned = c.sumStaticRepeatPasses = c.sumStaticRepeatCasters = 0;
    }
  }
  u64 rows = 0x9E3779B97F4A7C15ull;
  u64 world = 0x9E3779B97F4A7C15ull;
  if (const u8 *file = dev.Bytes(dev::kVsFloatConstants, 112)) {
    for (u32 i = 16; i < 28; ++i) {
      u32 raw;
      std::memcpy(&raw, file + 4 * i, 4);
      world ^= raw + 0x9E3779B97F4A7C15ull + (world << 6) + (world >> 2);
    }
    for (u32 i = 0; i < 16; ++i) {
      u32 raw;
      std::memcpy(&raw, file + 4 * i, 4);
      rows ^= raw + 0x9E3779B97F4A7C15ull + (rows << 6) + (rows >> 2);
    }
  }
  static u64 pass_rows = 0;
  if (c.casters && rows != pass_rows)
    ShadowPassClose(c);
  if (!c.casters)
    pass_rows = rows;
  const bool skinned = vs.floatConstantRegs > 16;
  u64 h = c.hash ? c.hash : rows;
  const auto mix = [&h](u64 v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
  mix(vs.hash);
  mix(streams[0].dataVa);
  mix((u64(lo) << 32) | hi);
  mix(world);
  if (skinned)
    mix(s.guest_frames);
  c.hash = h;
  if (!skinned) {
    u64 sh = c.staticHash ? c.staticHash : rows;
    sh ^= h + 0x9E3779B97F4A7C15ull + (sh << 6) + (sh >> 2);
    c.staticHash = sh;
  } else {
    c.skinned++;
  }
  c.casters++;
}

// WoS port (fix111): the per-stream vertex views of a draw, factored out of CaptureDraw so the
// render thread can build them (wos_defer_vertices) - the guest vertex reads, mirror lookups
// and copies were ~0.6 us of the game thread's ~2.6 us per draw.
static bool BuildVertexViews(VideoState &s, DrawPacket &pk, const InputLayout *layout,
                             const StreamInfo *streams, u32 lo, u32 hi, bool rect_list,
                             const RectExpansion *rect, u32 inst_mask, u32 inst_n, u64 vs_hash) {
  // fix111: with wos_defer_vertices the render thread builds most draws' views and the game
  // thread the rest (UP data, morph streams): the vertex-mirror cache is shared, so locked
  static std::mutex mirror_mutex;
  std::unique_lock<std::mutex> mirror_lock(mirror_mutex, std::defer_lock);
  if (wos_defer_vertices_cached())
    mirror_lock.lock();
  for (u32 S = 0; S <= pk.max_slot; ++S) {
    pk.slots[S] = plume::RenderInputSlot(S, streams[S].stride);
    if (!(layout->streamMask & (1u << S))) {
      pk.views[S] = plume::RenderVertexBufferView(
          plume::RenderBufferReference(pk.zero.buffer, pk.zero.offset), 4096);
      pk.slots[S] = plume::RenderInputSlot(S, 0);
      continue;
    }
    const StreamInfo &st_info = streams[S];
    UploadAlloc va;
    if (rect_list) {
      if (!UploadBytes(rect->vertices[S].data(), rect->vertices[S].size(), 16, &va)) {
        Dropped("upload ring exhausted (vertices)", 0x600D);
        return false;
      }
      pk.views[S] = plume::RenderVertexBufferView(
          plume::RenderBufferReference(va.buffer, va.offset), static_cast<u32>(va.size));
      continue;
    }
    if (inst_n && st_info.stride && st_info.data) {
      // fix108: one record per guest vertex lo..hi, from the mesh or the instance element
      const bool per_instance = (inst_mask >> S) & 1u;
      const u32 stride = st_info.stride;
      const u64 count = u64(hi) - lo + 1;
      const u64 bytes = count * stride;
      if (bytes > 64ull * 1024 * 1024 || !UploadAllocate(bytes, 16, &va)) {
        Dropped("upload ring exhausted (instanced vertices)", 0x600D);
        return false;
      }
      const u64 avail = st_info.sizeBytes ? st_info.sizeBytes / stride : ~0ull;
      PerfScope copy_scope(s.perf.vertex_copy_ms);
      for (u64 j = 0; j < count; ++j) {
        const u64 h = u64(lo) + j;
        const u64 inst = h / inst_n;
        const u64 e = per_instance ? inst : h - inst * inst_n;
        u8 *dst = va.cpu + j * stride;
        if (e < avail)
          CopyVertexBytes(dst, st_info.data + e * stride, stride);
        else
          std::memset(dst, 0, stride);
      }
      s.perf.vertex_bytes += bytes;
      pk.views[S] = plume::RenderVertexBufferView(plume::RenderBufferReference(va.buffer, va.offset),
                                                  static_cast<u32>(bytes));
      continue;
    }
    const u64 first = u64(lo) * st_info.stride;
    u64 bytes = (u64(hi) - lo + 1) * st_info.stride;
    if (st_info.sizeBytes && first + bytes > st_info.sizeBytes) {
      if (first >= st_info.sizeBytes) {
        // WoS port (fix41): these grow during play (fix40: x145 by 40 s into a level) - in the
        // frames with buildings missing. Which stream, and how far outside
        static u32 detail = 0;
        if (detail < 40) {
          ++detail;
          EOT_WARN("[drop] stream {} va {:#x} {} bytes stride {} obj {:#x}: vertices {}..{} ({} bytes "
                   "from {}) vs {} | vs {:016x} decl-streams {:#x} indexed {} count {}",
                   S, st_info.dataVa, st_info.sizeBytes, st_info.stride, st_info.objectVa, lo, hi,
                   bytes, first, st_info.sizeBytes / std::max(1u, st_info.stride), vs_hash,
                   layout->streamMask, pk.indexed, pk.indexed ? pk.index_count : pk.vertexCount);
        }
        Dropped("vertex range outside the stream", 0x600E);
        return false;
      }
      bytes = st_info.sizeBytes - first;
    }
    if (bytes > 64ull * 1024 * 1024) {
      Dropped("vertex range implausibly large", 0x600F);
      return false;
    }
    const bool morph = (layout->morphStreams >> S) & 1u;
    if (morph)
      NoteMorphStream(s, st_info, first, bytes);
    if (const VertexMirror *mirror = morph ? nullptr : GetVertexMirror(s, st_info, first, bytes)) {
      auto &pool = vertex_mirrors().pool;
      const u64 epoch = pool.flushEpoch.load(std::memory_order_acquire);
      if (mirror->residentEpoch != epoch || !mirror->resident) {
        mirror->resident = PoolResidentBuffer(s, pool, mirror->buffer, mirror->offset, bytes,
                                              &mirror->residentCapacity);
        mirror->residentEpoch = epoch;
      }
      if (pk.max_slot == 0 && mirror->residentCapacity && mirror->offset % st_info.stride == 0 &&
          mirror->residentCapacity <= 0xFFFFFFFFull) {
        pk.views[S] = plume::RenderVertexBufferView(
            plume::RenderBufferReference(mirror->resident, 0),
            static_cast<u32>(mirror->residentCapacity));
        pk.host_base_vertex += static_cast<i32>(mirror->offset / st_info.stride);
        continue;
      }
      pk.views[S] = plume::RenderVertexBufferView(
          plume::RenderBufferReference(mirror->resident, mirror->offset),
          static_cast<u32>(bytes));
      continue;
    }
    if (!UploadAllocate(bytes, 16, &va)) {
      Dropped("upload ring exhausted (vertices)", 0x600D);
      return false;
    }
    {
      PerfScope copy_scope(s.perf.vertex_copy_ms);
      CopyVertexBytes(va.cpu, st_info.data + first, static_cast<u32>(bytes));
    }
    s.perf.vertex_bytes += bytes;
    pk.views[S] = plume::RenderVertexBufferView(plume::RenderBufferReference(va.buffer, va.offset),
                                                static_cast<u32>(bytes));
  }
  return true;
}

// WoS port (fix117): wos_strip_fast_path - the city's window strips (63% of a city frame's
// draws, each a quad list of 1-13 quads) cost what any draw costs. Two shortcuts for quad
// lists that go through the render thread:
//
// 1 - the shared index pattern. A quad list had its own index list built (two heap
//     allocations), scanned for its vertex range and uploaded, every draw. The pattern of a
//     quad list is always (0 1 2, 0 2 3) + 4q: one index buffer a frame serves them all, with
//     the draw's first vertex as the base.
// 2 - the repeat. What a draw derives from its device window - shaders, render targets, the
//     input layout, the vertex streams - is a function of the window's bytes (and of the
//     declaration, the shader generation and the upload ring's epoch). A quad list whose
//     window, after the stream overlay, is byte for byte the last quad list's takes what
//     that one derived; only its vertices (first, count) and its constants are its own.
// 3 - as 1, with the repeat only predicted and checked against the full work: the log says
//     how often it would have differed ("[strip-fast] ... mismatches").
// The capture's statics are safe here: the render-thread path holds the queue's lock.
constexpr u32 kQuadPatternVertices = 8192;  // fix118: merged strip runs (wos_strip_merge)

bool QuadPatternIndices(VideoState &s, UploadAlloc *out) {
  static UploadAlloc cached[kNumFrames];
  static u64 cached_frame[kNumFrames] = {};
  static bool cached_valid[kNumFrames] = {};
  const u32 slot = s.recording_slot();
  if (cached_valid[slot] && cached_frame[slot] == UploadRingEpoch()) {
    *out = cached[slot];
    return true;
  }
  constexpr u32 kQuads = kQuadPatternVertices / 4;
  if (!UploadAllocate(u64(kQuads) * 6 * 4, 4, out))
    return false;
  u32 pattern[6 * 16];
  u8 *dst = out->cpu;
  for (u32 q = 0; q < kQuads; q += 16) {
    for (u32 k = 0; k < 16; ++k) {
      const u32 v = 4 * (q + k);
      u32 *w = pattern + 6 * k;
      w[0] = v;
      w[1] = v + 1;
      w[2] = v + 2;
      w[3] = v;
      w[4] = v + 2;
      w[5] = v + 3;
    }
    std::memcpy(dst, pattern, sizeof pattern);
    dst += sizeof pattern;
  }
  cached[slot] = *out;
  cached_frame[slot] = UploadRingEpoch();
  cached_valid[slot] = true;
  return true;
}

namespace {
constexpr u32 StripBlockBytes() {
  u32 n = 0;
  for (const DeviceWindow::Block &b : DeviceWindow::kBlocks)
    n += b.size;
  return n;
}
struct StripRepeat {
  bool valid = false;
  u64 frame = 0, generation = 0, epoch = 0;
  u32 decl = 0;
  u8 blocks[StripBlockBytes()];
  GuestShader *vs = nullptr, *ps = nullptr;
  u32 vs_va = 0, ps_va = 0;
  const InputLayout *layout = nullptr;
  TargetWords targets;
  u32 strides[16] = {};
  u32 max_slot = 0;
  bool need_zero = false;
  DrawPacket::DeferredStream streams[16];
  u64 draws = 0, hits = 0, mismatches = 0, logged = 0;
};
StripRepeat g_strip_repeat;
bool g_capture_queued = false;  // CaptureDraw runs under the render queue's lock

inline bool StripBlocksEqual(const u8 *image, const u8 *saved) {
  for (const DeviceWindow::Block &b : DeviceWindow::kBlocks) {
    if (std::memcmp(image + b.base, saved, b.size) != 0)
      return false;
    saved += b.size;
  }
  return true;
}
inline void StripBlocksSave(const u8 *image, u8 *saved) {
  for (const DeviceWindow::Block &b : DeviceWindow::kBlocks) {
    std::memcpy(saved, image + b.base, b.size);
    saved += b.size;
  }
}
}

bool CaptureDraw(VideoState &s, u32 device_va, u32 prim, GeometryPlan &geom,
                 const u8 *device_image, DrawPacket &pk) {
  static u32 capture_count = 0;
  const bool timed = (++capture_count & 15u) == 0;
  u64 lap_t0 = timed ? PerfNow() : 0;
  auto lap = [&](f64 &acc) {
    if (!timed)
      return;
    const u64 t1 = PerfNow();
    acc += 16.0 * static_cast<f64>(t1 - lap_t0) * PerfMsPerTick();
    lap_t0 = t1;
  };
  // WoS port (fix63): finer split of the game-thread capture, per draw, logged every 64k draws
  // (sampled with the laps: every 16th draw). Finds what can move to the render thread.
  static f64 split_ms[10] = {};
  static u32 split_draws = 0;
  u64 split_t = lap_t0;
  auto mark = [&](u32 i) {
    if (!timed)
      return;
    const u64 t1 = PerfNow();
    split_ms[i] += 16.0 * static_cast<f64>(t1 - split_t) * PerfMsPerTick();
    split_t = t1;
  };
  if (++split_draws == 65536) {
    static const char *const kNames[10] = {"window", "overlay", "shaders", "targets", "layout",
                                           "readstream", "index+probes", "constants", "vertices",
                                           "rest"};
    std::string line;
    for (u32 i = 0; i < 10; ++i)
      line += std::format(" {} {:.2f}", kNames[i], split_ms[i] * 1000.0 / split_draws);
    EOT_INFO("[capture-split] us per draw:{}", line);
    std::fill(std::begin(split_ms), std::end(split_ms), 0.0);
    split_draws = 0;
  }
  pk.device_va = device_va;
  pk.deferVertices = false;  // fix111
  pk.deferFailed = false;
  pk.deferConstSwap[0] = pk.deferConstSwap[1] = false;
  pk.prim = prim;
  pk.indexed = geom.indexed;
  pk.rectList = geom.rectList;
  pk.topology = geom.topology;
  pk.vertexCount = geom.vertexCount;
  pk.hasCached = false;
  pk.index_alloc = UploadAlloc{};
  pk.index_count = 0;
  pk.host_base_vertex = 0;
  pk.zero = UploadAlloc{};
  DeviceView dev = Device(device_va);
  const u8 *regs = nullptr;
  if (device_image) {
    dev.snapshot = device_image;
    dev.snapshotSize = kDeviceSnapshotBytes;
    regs = device_image;
  } else if (const u8 *live = mem::at<u8>(device_va)) {
    dev.snapshot = live;
    dev.snapshotSize = dev::kDeviceSize;
    regs = live;
  }
  if (!regs) {
    Dropped("device unreadable", 0x6000);
    return false;
  }
  pk.window.Capture(regs);
  mark(0);
  // WoS port (fix34): what the GPU would draw with - the stream's fetch constants and
  // render state - over D3D's shadow (UP draws got theirs when queued: device_image)
  if (!device_image) {
    wos_regs_overlay(device_va, pk.window.image, kDeviceSnapshotBytes, 1);
    // and the capture reads it from here on (vertex streams included): the window's blocks
    // from the overlaid copy, everything else from the live device as before
    dev.snapshot = pk.window.image;
    dev.snapshotSize = kDeviceSnapshotBytes;
    dev.pages = DeviceWindow::PageTable();
  }

  mark(1);
  // fix117: the repeat (see above)
  static const int strip_mode = wos_strip_fast_path();
  StripRepeat &rp = g_strip_repeat;
  bool rp_eligible = false, rp_predicted = false;
  if (strip_mode >= 2 && geom.quadPattern && g_capture_queued && !device_image) {
    static const bool blocked = wos_diag_extra() || wos_defer_verify() || !wos_defer_vertices() ||
                                Settings::MotionVectors();
    // (the dump / diagnostic frame blocks a repeat skips only log)
    rp_eligible = !blocked;
  }
  if (rp_eligible) {
    if ((++rp.draws & 0x3FFFFu) == 0) {
      EOT_INFO("[strip-fast] {} quad-list draws, {} repeats ({:.0f}%){}", rp.draws, rp.hits,
               100.0 * rp.hits / rp.draws,
               strip_mode >= 3 ? std::format(" - checked, {} mismatches", rp.mismatches) : std::string());
    }
    rp_predicted = rp.valid && rp.frame == s.captured_presents &&
                   rp.generation == s.shader_generation.load(std::memory_order_acquire) &&
                   rp.epoch == UploadRingEpoch() && rp.decl == dev.U32(dev::kVertexDeclaration) &&
                   StripBlocksEqual(pk.window.image, rp.blocks);
    // the render targets' headers are the game's memory, not the window's: predicated tiling
    // rewrites them in place between passes
    if (rp_predicted) {
      u32 words[5];
      for (u32 i = 0; i < rp.targets.colorCount && rp_predicted; ++i)
        rp_predicted = ReadSurfaceHeaderWords(rp.targets.colorVa[i], words) &&
                       std::memcmp(words, rp.targets.colorWords[i], sizeof words) == 0;
      if (rp_predicted && rp.targets.depthVa)
        rp_predicted = ReadSurfaceHeaderWords(rp.targets.depthVa, words) &&
                       std::memcmp(words, rp.targets.depthWords, sizeof words) == 0;
    }
    if (rp_predicted)
      ++rp.hits;
    if (rp_predicted && strip_mode == 2) {
      pk.vs = rp.vs;
      pk.ps = rp.ps;
      pk.vs_va = rp.vs_va;
      pk.ps_va = rp.ps_va;
      pk.hasCameraVP = false;
      pk.targets = rp.targets;
      pk.layout = rp.layout;
      std::memcpy(pk.strides, rp.strides, sizeof pk.strides);
      if (!QuadPatternIndices(s, &pk.index_alloc)) {
        Dropped("upload ring exhausted (indices)", 0x600A);
        return false;
      }
      pk.index_count = geom.vertexCount / 4 * 6;
      pk.host_base_vertex = 0;
      pk.velocity = false;
      static const bool rp_defer_consts = wos_defer_constants();
      const bool rp_raw = rp_defer_consts && RenderThreadActive();
      if (!UploadFloatFile(s, dev, dev::kVsFloatConstants, 0, rp.vs->floatConstantRegs,
                           s.vs_float_constants_stale, &pk.vs_consts,
                           rp_raw ? &pk.deferConstSwap[0] : nullptr) ||
          !UploadFloatFile(s, dev, dev::kPsFloatConstants, 1, rp.ps ? rp.ps->floatConstantRegs : 16u,
                           s.ps_float_constants_stale, &pk.ps_consts,
                           rp_raw ? &pk.deferConstSwap[1] : nullptr)) {
        Dropped("constant upload failed", 0x6010);
        return false;
      }
      s.vs_float_constants_stale = 0;
      s.ps_float_constants_stale = 0;
      pk.max_slot = rp.max_slot;
      if (rp.need_zero && !UploadZeroBuffer(s, &pk.zero)) {
        Dropped("upload ring exhausted (zero buffer)", 0x600C);
        return false;
      }
      pk.deferVertices = true;
      pk.dLo = geom.startVertex;
      pk.dHi = geom.startVertex + geom.vertexCount - 1;
      pk.dInstMask = 0;
      pk.dInstN = 0;
      for (u32 S = 0; S <= rp.max_slot && S < 16; ++S)
        pk.dStreams[S] = rp.streams[S];
      return true;
    }
  }
  const u32 vs_va = dev.U32(dev::kVertexShader);
  const u32 ps_va = dev.U32(dev::kPixelShader);
  struct ShaderLookupCache {
    u64 generation = 0;
    u32 vs_va = 0, ps_va = 0;
    GuestShader *vs = nullptr, *ps = nullptr;
  };
  static thread_local ShaderLookupCache slc;
  const u64 shader_gen = s.shader_generation.load(std::memory_order_acquire);
  if (slc.generation != shader_gen)
    slc = ShaderLookupCache{shader_gen};
  GuestShader *vs = slc.vs_va == vs_va && slc.vs ? slc.vs : nullptr;
  if (!vs) {
    vs = FindGuestShader(s, vs_va);
    if (!vs && vs_va)
      vs = RegisterGuestShader(s, vs_va, false);
    slc.vs_va = vs_va;
    slc.vs = vs;
  }
  GuestShader *ps = slc.ps_va == ps_va && slc.ps ? slc.ps : nullptr;
  if (!ps) {
    ps = FindGuestShader(s, ps_va);
    if (!ps && ps_va)
      ps = RegisterGuestShader(s, ps_va, true);
    slc.ps_va = ps_va;
    slc.ps = ps;
  }
  if (!vs) {
    u32 n;
    if (DiagShouldLog(0x6021, &n))
      EOT_WARN("[draw] no VS: vs_va={:#x} ps_va={:#x} prim={} {} n={} rt0={:#x} decl={:#x} "
               "stream0={:#x} frame={}",
               vs_va, ps_va, prim, geom.indexed ? "idx" : "vtx", geom.vertexCount,
               dev.U32(dev::kRenderTarget0), dev.U32(dev::kVertexDeclaration),
               dev.U32(dev::kStreamObject0), s.guest_frames);
    Dropped(vs_va ? "vertex shader object unreadable" : "no vertex shader bound", 0x6001);
    return false;
  }
  if (ps_va && !ps) {
    Dropped("pixel shader object unreadable", 0x6013);
    return false;
  }
  if (!vs->entry || (ps && !ps->entry)) {
    trace::Bump(trace::Counter::ShaderMiss);
    auto note_miss = [](GuestShader &sh) {
      if (sh.entry || sh.cacheMissLogged)
        return;
      sh.cacheMissLogged = true;
      EOT_WARN("[shaders] {} {:#x} hash {:016x} is not in the shader cache",
               sh.isPixel ? "ps" : "vs", sh.va, sh.hash);
    };
    note_miss(*vs);
    if (ps)
      note_miss(*ps);
    Dropped("shader cache miss", 0x6002);
    return false;
  }
  pk.vs = vs;
  pk.ps = ps;
  // WoS port (fix66): building LOD flicker hunt. Draws per (vs, ps) pair per frame; a pair
  // whose count jumps one way and straight back (A-B-A over three frames, >= 8 draws and
  // >= 25%) is logged - which building versions the game alternates between at the edge.
  // fix78: only with wos_diagnostics (a mutex + hash map per draw on the game thread)
  static const bool diag_extra = wos_diag_extra();
  if (diag_extra) {
    struct FlipCount {
      u32 c[3] = {};  // this frame, last frame, the one before
    };
    static std::unordered_map<u64, FlipCount> counts;
    static u64 frame = ~0ull;
    static u32 logged = 0;
    static std::mutex flip_mutex;  // the main thread and JQ worker 1 both draw
    std::lock_guard flip_lock(flip_mutex);
    if (s.guest_frames != frame) {
      if (frame != ~0ull && logged < 600) {
        u32 lines = 0;
        for (auto &[key, fc] : counts) {
          // fc.c[0] = the frame that just ended, c[1] before it, c[2] before that
          const u32 a = fc.c[2], b = fc.c[1], c = fc.c[0];
          auto big = [](u32 x, u32 y) {
            const u32 d = x > y ? x - y : y - x;
            return d >= 8 && d * 4 >= std::max(x, y);
          };
          if (big(a, b) && big(b, c) && ((b > a) == (b > c)) && lines < 6) {
            EOT_INFO("[lod-flip] frame {}: vs {:#x} ps {:#x} draws {} -> {} -> {}", frame,
                     static_cast<u32>(key >> 32), static_cast<u32>(key), a, b, c);
            ++lines;
            ++logged;
          }
        }
      }
      for (auto it = counts.begin(); it != counts.end();) {
        auto &fc = it->second;
        fc.c[2] = fc.c[1];
        fc.c[1] = fc.c[0];
        fc.c[0] = 0;
        if (!fc.c[1] && !fc.c[2])
          it = counts.erase(it);
        else
          ++it;
      }
      frame = s.guest_frames;
    }
    ++counts[(u64(vs_va) << 32) | ps_va].c[0];
  }
  pk.hasCameraVP = false;
  // WoS port: the camera matrix comes from an Edge of Time global (taa::kViewProjectionVa);
  // Web of Shadows has none located, so no camera tracking (TAA is off on the PS5 anyway).
  if (false && (Settings::Taa() || Settings::DiagVerbosity() >= 2)) {
    struct CameraByTarget {
      u32 rt0 = 0;
      float vp[16] = {};
      float prevVp[16] = {};
      bool prevValid = false;
      u64 frame = ~0ull;
      bool skip = false;
    };
    static CameraByTarget cameras[4];
    static u32 camera_next = 0;
    static const CameraByTarget *camera_last = nullptr;
    static u64 capture_frame = 0;
    static u64 last_present_count = ~0ull;
    if (s.captured_presents != last_present_count) {
      last_present_count = s.captured_presents;
      ++capture_frame;
    }
    const u32 rt0 = dev.U32(dev::kRenderTarget0);
    const u32 vte = dev.U32(dev::kVteControl);
    const bool projected = (vte & 5) == 5;
    const float xs = std::fabs(dev.F32(dev::kVportXScale)), ys = std::fabs(dev.F32(dev::kVportYScale));
    const bool frame_sized = std::fabs(xs * 2.0f - static_cast<float>(kGuestRenderWidth)) < 1.0f &&
                             std::fabs(ys * 2.0f - static_cast<float>(kGuestRenderHeight)) < 1.0f;
    if (projected && !geom.rectList && frame_sized && rt0) {
      if (const auto *m = eot::mem::at<eot::be<float>>(taa::kViewProjectionVa)) {
        CameraByTarget *slot = nullptr;
        for (CameraByTarget &c : cameras)
          if (c.rt0 == rt0)
            slot = &c;
        if (!slot)
          slot = &cameras[camera_next++ % 4];
        if (slot->rt0 != rt0) {
          slot->prevValid = false;
          slot->skip = false;
        }
        slot->rt0 = rt0;
        if (slot->frame != capture_frame) {
          if (slot->frame != ~0ull) {
            std::memcpy(slot->prevVp, slot->vp, sizeof(slot->prevVp));
            slot->prevValid = true;
          }
          slot->frame = capture_frame;
          for (u32 i = 0; i < 16; ++i)
            slot->vp[i] = m[i];
          const float motion =
              slot->prevValid
                  ? taa::CameraMotionPixels(slot->prevVp, slot->vp, static_cast<float>(InternalRenderWidth()),
                                            static_cast<float>(InternalRenderHeight()))
                  : 0.0f;
          slot->skip = taa::FastCameraGate(static_cast<u32>(slot - cameras), motion,
                                           static_cast<float>(InternalRenderWidth()));
        }
        camera_last = slot;
      }
    }
    if (camera_last && camera_last->frame == capture_frame)
      pk.taaSkip = camera_last->skip;
    if (ps && taa::IsSceneConsumer(ps->hash)) {
      const CameraByTarget *pick = nullptr;
      for (const CameraByTarget &c : cameras)
        if (c.rt0 && c.rt0 == rt0)
          pick = &c;
      if (!pick)
        pick = camera_last;
      if (pick && pick->rt0) {
        std::memcpy(pk.cameraVP, pick->vp, sizeof(pk.cameraVP));
        pk.hasCameraVP = true;
        pk.taaSkip = pick->skip;
      }
    }
  }
  pk.vs_va = vs_va;
  pk.ps_va = ps_va;

  mark(2);
  if (!CaptureTargetWords(dev, pk.targets)) {
    Dropped("no render target or depth surface bound", 0x6003);
    return false;
  }

  mark(3);
  const InputLayout *layout = GetInputLayout(s, *vs, dev.U32(dev::kVertexDeclaration));
  if (!layout) {
    Dropped("no input layout for the bound declaration", 0x6004);
    return false;
  }
  pk.layout = layout;
  mark(4);
  lap(s.perf.setup_ms);
  StreamInfo streams[16];
  for (u32 S = 0; S < 16; ++S) {
    if (!(layout->streamMask & (1u << S)))
      continue;
    if (S == 0 && geom.stream0OverrideVa) {
      streams[0].stream = 0;
      streams[0].stride = geom.stream0OverrideStride;
      streams[0].data = mem::at<u8>(geom.stream0OverrideVa);
      streams[0].sizeBytes = geom.vertexCount * geom.stream0OverrideStride;
      if (!streams[0].data || !streams[0].stride) {
        Dropped("BeginVertices data unreadable", 0x6015);
        return false;
      }
      continue;
    }
    if (!ReadStream(dev, S, streams[S])) {
      Dropped("stream source unreadable", 0x6005);
      return false;
    }
    if (layout->streamExtent[S] > streams[S].stride) {
      u32 n;
      if (DiagShouldLog(0x6006 + S, &n))
        EOT_WARN("[draw] stream {} stride {} smaller than the declaration extent {}", S,
                 streams[S].stride, layout->streamExtent[S]);
    }
  }
  for (u32 S = 0; S < 16; ++S)
    pk.strides[S] = streams[S].stride;
  mark(5);

  u32 lo = 0, hi = 0;
  RectExpansion rect;
  if (geom.rectList) {
    std::vector<u32> guest_vertices;
    if (geom.indexed) {
      guest_vertices = geom.indices;
      for (auto &v : guest_vertices)
        v = static_cast<u32>(static_cast<i32>(v) + geom.baseVertex);
    } else {
      guest_vertices.resize(geom.vertexCount);
      for (u32 i = 0; i < geom.vertexCount; ++i)
        guest_vertices[i] = geom.startVertex + i;
    }
    if (!ExpandRectList(*layout, streams, layout->streamMask, guest_vertices, rect) ||
        rect.indices.empty()) {
      Dropped("rect list expansion failed", 0x6009);
      return false;
    }
    if (!UploadBytes(rect.indices.data(), rect.indices.size() * 4, 4, &pk.index_alloc)) {
      Dropped("upload ring exhausted (indices)", 0x600A);
      return false;
    }
    pk.index_count = static_cast<u32>(rect.indices.size());
  } else if (geom.cached) {
    pk.hasCached = true;
    pk.cached = *geom.cached;
    lo = static_cast<u32>(static_cast<i32>(geom.cached->lo) + geom.baseVertex);
    hi = static_cast<u32>(static_cast<i32>(geom.cached->hi) + geom.baseVertex);
    pk.index_count = geom.cached->count;
    pk.host_base_vertex = geom.baseVertex - static_cast<i32>(lo);
  } else if (geom.quadPattern) {
    // fix117: the shared quad pattern, from the draw's first vertex
    lo = geom.startVertex;
    hi = geom.startVertex + geom.vertexCount - 1;
    if (!QuadPatternIndices(s, &pk.index_alloc)) {
      Dropped("upload ring exhausted (indices)", 0x600A);
      return false;
    }
    pk.index_count = geom.vertexCount / 4 * 6;
    pk.host_base_vertex = geom.baseVertex - static_cast<i32>(lo);
  } else if (geom.indexed) {
    if (geom.indices.empty()) {
      Dropped("empty index list", 0x600B);
      return false;
    }
    lo = 0xFFFFFFFFu;
    hi = 0;
    for (u32 v : geom.indices) {
      const u32 gv = static_cast<u32>(static_cast<i32>(v) + geom.baseVertex);
      lo = std::min(lo, gv);
      hi = std::max(hi, gv);
    }
    if (!UploadBytes(geom.indices.data(), geom.indices.size() * 4, 4, &pk.index_alloc)) {
      Dropped("upload ring exhausted (indices)", 0x600A);
      return false;
    }
    pk.index_count = static_cast<u32>(geom.indices.size());
    s.perf.index_bytes += u64(pk.index_count) * 4;
    pk.host_base_vertex = geom.baseVertex - static_cast<i32>(lo);
  } else {
    lo = geom.startVertex;
    hi = geom.startVertex + geom.vertexCount - 1;
  }

  // WoS port (fix87): the ground draws that leave a void (vs 8e6191df / 64abd48f) and the
  // dark road (vs 2e02f7c3): their geometry as the CPU sees it, once per armed dump frame -
  // index range, stream, raw POSITION0 of a few vertices, the matrices this VS family uses
  // (viewProj c0-3, world c4-7, decompress scale/bias c21/c22) and the CPU-computed clip
  // position, plus every state register that can make a draw rasterise nothing.
  {
    static i32 probe_for = -1;
    static u32 probe_n = 0;
    if (probe_for != Settings::DumpFrame()) {
      probe_for = Settings::DumpFrame();
      probe_n = 0;
    }
    const u64 h = vs->hash;
    // (fix88) each shader's gDecompPosScale / gDecompPosBias registers
    u32 sreg = 0, breg = 0;
    if (h == 0x8e6191dfe633885aull)
      sreg = 21, breg = 22;
    else if (h == 0x64abd48f1982edf3ull)
      sreg = 24, breg = 25;
    else if (h == 0x2e02f7c32da7d5acull)
      sreg = 8, breg = 9;
    const bool target = sreg != 0;
    if (target && probe_n < 40 && Settings::DumpFrame() > 0 &&
        s.guest_frames + 1 >= static_cast<u64>(Settings::DumpFrame())) {
      ++probe_n;
      auto C = [&](u32 r, u32 c) { return dev.F32(dev::kVsFloatConstants + 16 * r + 4 * c); };
      const plume::RenderInputElement *pos = nullptr;
      for (const auto &e : layout->elements)
        if (e.semanticName && std::string_view(e.semanticName) == "POSITION" &&
            (e.semanticIndex & 0xFF) == 0)
          pos = &e;
      EOT_INFO("[probe] f{} vs {:016x} ps {:016x} idx {} cached {} count {} lo {} hi {} base {} hostbase {} | "
               "mode {:#x} vte {:#x} vtx {:#x} clip {:#x} depth {:#x} cc {:#x} mask {:#x} | vport "
               "x {:g}/{:g} y {:g}/{:g} z {:g}/{:g} | poly f {:g}/{:g} b {:g}/{:g} | win {:#x} "
               "wsc {:#x}-{:#x} ssc {:#x}-{:#x}",
               s.guest_frames + 1, h, ps ? ps->hash : 0ull, geom.indexed, geom.cached != nullptr,
               geom.cached ? geom.cached->count : static_cast<u32>(geom.indices.size()), lo, hi,
               geom.baseVertex, pk.host_base_vertex, dev.U32(dev::kModeControl),
               dev.U32(dev::kVteControl), dev.U32(dev::kVtxControl), dev.U32(dev::kClipControl),
               dev.U32(dev::kDepthControl), dev.U32(dev::kColorControl), dev.U32(dev::kColorMask),
               dev.F32(dev::kVportXScale), dev.F32(dev::kVportXOffset), dev.F32(dev::kVportYScale),
               dev.F32(dev::kVportYOffset), dev.F32(dev::kVportZScale), dev.F32(dev::kVportZOffset),
               dev.F32(dev::kPolyOffsetFrontScale), dev.F32(dev::kPolyOffsetFrontOffset),
               dev.F32(dev::kPolyOffsetBackScale), dev.F32(dev::kPolyOffsetBackOffset),
               dev.U32(dev::kWindowOffset), dev.U32(dev::kWindowScissorTL),
               dev.U32(dev::kWindowScissorBR), dev.U32(dev::kScreenScissorTL),
               dev.U32(dev::kScreenScissorBR));
      std::string mats;
      for (u32 r : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, sreg, breg})
        mats += std::format(" c{}=({:.6g},{:.6g},{:.6g},{:.6g})", r, C(r, 0), C(r, 1), C(r, 2),
                            C(r, 3));
      EOT_INFO("[probe]   consts{}", mats);
      if (pos && pos->slotIndex < 16) {
        const StreamInfo &si = streams[pos->slotIndex];
        EOT_INFO("[probe]   POSITION0 s{} +{} fmt {} | stream va {:#x} stride {} size {} ({} verts)",
                 pos->slotIndex, pos->alignedByteOffset, static_cast<u32>(pos->format), si.dataVa,
                 si.stride, si.sizeBytes, si.stride ? si.sizeBytes / si.stride : 0u);
        std::vector<u32> picks;
        if (!geom.indices.empty()) {
          for (u32 k = 0; k < 6 && k < geom.indices.size(); ++k)
            picks.push_back(static_cast<u32>(static_cast<i32>(geom.indices[k]) + geom.baseVertex));
        }
        if (hi >= lo)
          for (u32 k = 0; k <= 4; ++k)
            picks.push_back(lo + static_cast<u32>((u64(hi - lo) * k) / 4));
        for (u32 v : picks) {
          const u64 off = u64(v) * si.stride + pos->alignedByteOffset;
          if (!si.data || !si.stride || off + 8 > si.sizeBytes) {
            EOT_INFO("[probe]   v{} out of stream ({} + 8 > {})", v, off, si.sizeBytes);
            continue;
          }
          const u8 *pv = si.data + off;
          float p[4] = {0, 0, 0, 1};
          u32 raw0 = rex::memory::load_and_swap<u32>(pv), raw1 = rex::memory::load_and_swap<u32>(pv + 4);
          if (pos->format == plume::RenderFormat::R16G16B16A16_SNORM) {
            for (u32 c = 0; c < 4; ++c) {
              const i16 q = static_cast<i16>(rex::memory::load_and_swap<u16>(pv + 2 * c));
              p[c] = std::max(-1.0f, q / 32767.0f);
            }
          } else if (pos->format == plume::RenderFormat::R32G32B32_FLOAT ||
                     pos->format == plume::RenderFormat::R32G32B32A32_FLOAT) {
            for (u32 c = 0; c < 3; ++c)
              p[c] = std::bit_cast<float>(rex::memory::load_and_swap<u32>(pv + 4 * c));
          }
          float o[4], w[4], clip[4];
          for (u32 c = 0; c < 4; ++c)
            o[c] = p[c] * C(sreg, c) + C(breg, c);
          for (u32 r = 0; r < 4; ++r)
            w[r] = o[0] * C(4 + r, 0) + o[1] * C(4 + r, 1) + o[2] * C(4 + r, 2) + o[3] * C(4 + r, 3);
          for (u32 r = 0; r < 4; ++r)
            clip[r] = w[0] * C(r, 0) + w[1] * C(r, 1) + w[2] * C(r, 2) + w[3] * C(r, 3);
          EOT_INFO("[probe]   v{} raw {:08x} {:08x} pos ({:.4g},{:.4g},{:.4g},{:.4g}) obj ({:.4g},"
                   "{:.4g},{:.4g},{:.4g}) world ({:.5g},{:.5g},{:.5g}) clip ({:.4g},{:.4g},{:.4g},"
                   "{:.4g}) ndc ({:.3g},{:.3g},{:.3g})",
                   v, raw0, raw1, p[0], p[1], p[2], p[3], o[0], o[1], o[2], o[3], w[0], w[1], w[2],
                   clip[0], clip[1], clip[2], clip[3], clip[0] / clip[3], clip[1] / clip[3],
                   clip[2] / clip[3]);
        }
        // WoS port (fix89): the whole mesh for offline rasterising - every triangle's three
        // vertices in clip space, as computed on the CPU from guest memory. File: "WOSP" u32
        // version 1, u64 vs hash, u32 prim, u32 index count, u32 triangles, then 12 floats each
        {
          std::vector<u32> idx;
          const u32 prim_now = pk.prim;
          const u32 guest_count = geom.guestIndexCount;
          if (geom.cached) {
            if (ReadGuestIndices(dev.U32(dev::kIndexBuffer), geom.startIndex, guest_count, idx) &&
                prim_now == kPrimTriangleStrip)
              ExpandIndices(prim_now, idx, true);
          } else {
            idx = geom.indices;
          }
          const std::string path = std::format("/app0/userdata/wos_recomp/dump/f{}_probe{:02}_{:016x}.bin",
                                               s.guest_frames + 1, probe_n, h);
          ::mkdir("/app0/userdata/wos_recomp/dump", 0777);
          if (FILE *f = std::fopen(path.c_str(), "wb")) {
            const u32 tris = static_cast<u32>(idx.size() / 3);
            const u32 hdr[3] = {0x50534F57u, 1u, 0u};
            std::fwrite(hdr, 4, 2, f);
            std::fwrite(&h, 8, 1, f);
            const u32 meta[3] = {prim_now, guest_count, tris};
            std::fwrite(meta, 4, 3, f);
            for (u32 t = 0; t < tris; ++t) {
              float out[12] = {};
              for (u32 k = 0; k < 3; ++k) {
                const u32 v = static_cast<u32>(static_cast<i32>(idx[3 * t + k]) + geom.baseVertex);
                const u64 off = u64(v) * si.stride + pos->alignedByteOffset;
                float p[4] = {0, 0, 0, 1};
                if (si.data && si.stride && off + 8 <= si.sizeBytes &&
                    pos->format == plume::RenderFormat::R16G16B16A16_SNORM)
                  for (u32 c = 0; c < 4; ++c)
                    p[c] = std::max(-1.0f, static_cast<i16>(rex::memory::load_and_swap<u16>(
                                               si.data + off + 2 * c)) / 32767.0f);
                float o[4], w[4];
                for (u32 c = 0; c < 4; ++c)
                  o[c] = p[c] * C(sreg, c) + C(breg, c);
                for (u32 r = 0; r < 4; ++r)
                  w[r] = o[0] * C(4 + r, 0) + o[1] * C(4 + r, 1) + o[2] * C(4 + r, 2) +
                         o[3] * C(4 + r, 3);
                for (u32 r = 0; r < 4; ++r)
                  out[4 * k + r] = w[0] * C(r, 0) + w[1] * C(r, 1) + w[2] * C(r, 2) + w[3] * C(r, 3);
              }
              std::fwrite(out, 4, 12, f);
            }
            std::fclose(f);
            EOT_INFO("[probe]   mesh: prim {} {} guest indices -> {} triangles -> {}", prim_now,
                     guest_count, tris, path);
          }
        }
      }
    }
  }

  if (!geom.rectList && !wos_defer_vertices_cached()) {  // fix111: no use when deferring
    for (u32 S = 0; S < 16; ++S) {
      if (!(layout->streamMask & (1u << S)) || (S == 0 && geom.stream0OverrideVa))
        continue;
      const StreamInfo &st_info = streams[S];
      const u64 first = u64(lo) * st_info.stride;
      u64 bytes = (u64(hi) - lo + 1) * st_info.stride;
      if (st_info.sizeBytes && first + bytes > st_info.sizeBytes)
        bytes = first < st_info.sizeBytes ? st_info.sizeBytes - first : 0;
      if (bytes && bytes <= kVertexMirrorMaxBytes)
        PrefetchSampleProbes(st_info.data + first, bytes);
    }
  }

  if (!pk.targets.colorCount && pk.targets.depthVa && !ps && !geom.rectList &&
      !geom.stream0OverrideVa && ShadowTileWords(pk.targets.depthWords))
    NoteShadowCaster(s, dev, *vs, streams, lo, hi);

  pk.velocity = false;
  if (Settings::MotionVectors() && ps && !geom.rectList && vs->entry && ps->entry &&
      vs->entry->usesFloatConstants && VelocityCandidate(dev, pk.targets) &&
      VsVariantFor(vs->entry, ps->entry, false, true) == VsVariant::Velocity) {
    const StreamInfo &s0 = streams[0];
    u64 key = 0x51ED270B0E9A4C3Dull;
    auto mix = [&key](u64 v) { key ^= v + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2); };
    mix(geom.stream0OverrideVa ? geom.stream0OverrideVa : s0.objectVa);
    mix(s0.dataVa);
    mix((u64(lo) << 32) | hi);
    mix(static_cast<u32>(geom.baseVertex));
    mix((u64(vs_va) << 32) | ps_va);
    mix(pk.indexed ? pk.index_count : pk.vertexCount);
    const u32 regs = std::clamp(vs->floatConstantRegs, 16u, 256u);
    if (const u8 *guest_file = dev.Bytes(dev::kVsFloatConstants, regs * 16)) {
      UploadAlloc block;
      if (velocity::PrepareDraw(s, key, guest_file, regs, &block)) {
        pk.vs_consts = block;
        pk.velocity = true;
      }
    }
    lap(s.perf.const_float_ms);
  }

  mark(6);
  // fix111: wos_defer_constants - the byte swap on the render thread
  static const bool defer_consts = wos_defer_constants();
  const bool raw_ok = defer_consts && RenderThreadActive();
  pk.deferConstSwap[0] = pk.deferConstSwap[1] = false;
  if ((!pk.velocity && !UploadFloatFile(s, dev, dev::kVsFloatConstants, 0, vs->floatConstantRegs,
                                        s.vs_float_constants_stale, &pk.vs_consts,
                                        raw_ok ? &pk.deferConstSwap[0] : nullptr)) ||
      !UploadFloatFile(s, dev, dev::kPsFloatConstants, 1, ps ? ps->floatConstantRegs : 16u,
                       s.ps_float_constants_stale, &pk.ps_consts,
                       raw_ok ? &pk.deferConstSwap[1] : nullptr)) {
    Dropped("constant upload failed", 0x6010);
    return false;
  }
  if (!pk.velocity)
    s.vs_float_constants_stale = 0;
  s.ps_float_constants_stale = 0;
  lap(s.perf.const_float_ms);
  // WoS port (fix73): facade glass diagnostic. Every facade draw in a frame should see the same
  // camera (PS c0-c2 = viewInverse rows; .w = the camera position). A draw whose camera differs
  // from the frame's first facade draw is logged - a stray camera turns the reflections.
  if (ps && diag_extra) {
    const u64 h = ps->hash;
    if (h == 0x5acb3796e01eed3cull || h == 0x5f18a4e3ead2ec0cull || h == 0x6075f07454832b24ull ||
        h == 0xc447a6667bf82a6dull) {
      static u64 frame = ~0ull;
      static u8 ref[48];
      static u32 draws = 0, odd = 0, logged = 0;
      const u8 *c = dev.Bytes(dev::kPsFloatConstants, 48);
      if (c) {
        if (s.guest_frames != frame) {
          if (frame != ~0ull && odd && logged < 200) {
            ++logged;
            EOT_INFO("[glass] frame {}: {} of {} facade draws saw another camera", frame, odd, draws);
          }
          frame = s.guest_frames;
          std::memcpy(ref, c, 48);
          draws = odd = 0;
        }
        ++draws;
        if (std::memcmp(ref, c, 48) != 0) {
          if (odd++ == 0 && logged < 200) {
            auto f = [](const u8 *p, u32 i) {
              u32 v;
              std::memcpy(&v, p + 4 * i, 4);
              v = __builtin_bswap32(v);
              f32 r;
              std::memcpy(&r, &v, 4);
              return r;
            };
            EOT_INFO("[glass] frame {} draw {}: camera ({:.1f} {:.1f} {:.1f}) vs frame's ({:.1f} {:.1f} {:.1f})",
                     frame, draws, f(c, 3), f(c, 7), f(c, 11), f(ref, 3), f(ref, 7), f(ref, 11));
          }
        }
      }
    }
  }
  mark(7);

  u32 max_slot = 0;
  for (u32 S = 0; S < 16; ++S)
    if (layout->streamMask & (1u << S))
      max_slot = S;
  pk.max_slot = max_slot;
  const u32 prefix_mask = (1u << (max_slot + 1)) - 1;
  const bool need_zero = layout->needsSyntheticSlot || layout->streamMask != prefix_mask;
  if (need_zero && !UploadZeroBuffer(s, &pk.zero)) {
    Dropped("upload ring exhausted (zero buffer)", 0x600C);
    return false;
  }
  // fix108: shader instancing - N from the VS's instance_params (c6.y)
  static const bool instancing_on = wos_instancing();
  u32 inst_mask = 0, inst_n = 0;
  if (instancing_on && !geom.rectList && !geom.stream0OverrideVa && vs && hi >= lo)
    if ((inst_mask = InstanceStreamMask(*layout, vs->hash)) != 0) {
      const f32 n = dev.F32(dev::kVsFloatConstants + 16 * 6 + 4);
      if (n >= 1.0f && n <= 1048576.0f && n == std::floor(n)) {
        inst_n = static_cast<u32>(n);
      } else {
        inst_mask = 0;
      }
      static u32 logged = 0;
      if (logged < 24) {
        ++logged;
        EOT_INFO("[instancing] fix108: vs {:016x} instance streams {:#x} N {} (c6.y {}) guest "
                 "vertices {}..{}{}",
                 vs->hash, InstanceStreamMask(*layout, vs->hash), inst_n, n, lo, hi,
                 inst_mask ? "" : " - N not usable, drawn as before");
      }
    }
  // fix111: wos_defer_vertices - the stream reads and vertex views on the render thread
  static const bool defer_on = wos_defer_vertices();
  static const bool defer_verify = wos_defer_verify();
  const bool defer_this = defer_on && RenderThreadActive() && !geom.rectList && !geom.stream0OverrideVa &&
                          !layout->morphStreams && !pk.velocity;
  pk.deferVertices = defer_this;
  if (defer_this) {
    pk.dLo = lo;
    pk.dHi = hi;
    pk.dInstMask = inst_mask;
    pk.dInstN = inst_n;
    for (u32 S = 0; S <= max_slot; ++S) {
      DrawPacket::DeferredStream &d = pk.dStreams[S];
      const StreamInfo &si = streams[S];
      d.data = si.data;
      d.dataVa = si.dataVa;
      d.stride = si.stride;
      d.sizeBytes = si.sizeBytes;
      d.objectVa = si.objectVa;
      d.stream = si.stream;
      d.verifyHash = 0;
      if (defer_verify && (layout->streamMask & (1u << S)) && si.data && si.stride) {
        const u64 first = u64(lo) * si.stride;
        u64 bytes = (u64(hi) - lo + 1) * si.stride;
        if (si.sizeBytes && first + bytes > si.sizeBytes)
          bytes = first < si.sizeBytes ? si.sizeBytes - first : 0;
        if (bytes && bytes <= kVertexMirrorMaxBytes)
          d.verifyHash = XXH3_64bits(si.data + first, static_cast<size_t>(bytes)) | 1;
      }
    }
  } else if (!BuildVertexViews(s, pk, layout, streams, lo, hi, geom.rectList, &rect, inst_mask, inst_n,
                               vs->hash)) {
    return false;
  }
  lap(s.perf.stream_ms);
  mark(8);

  if (geom.rectList && streams[0].data && streams[0].stride >= 8 && Settings::DiagFrame() > 0 &&
      s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame())) {
    std::string verts;
    const u32 n_show = std::min<u32>(geom.vertexCount, 12u);
    for (u32 v = 0; v < n_show; ++v) {
      const u8 *p = streams[0].data + u64(v) * streams[0].stride;
      float f[4] = {0, 0, 0, 0};
      const u32 words = std::min<u32>(4u, streams[0].stride / 4u);
      for (u32 c = 0; c < words; ++c) {
        u32 raw;
        std::memcpy(&raw, p + 4 * c, 4);
        raw = (raw >> 24) | ((raw >> 8) & 0xFF00u) | ((raw << 8) & 0xFF0000u) | (raw << 24);
        std::memcpy(&f[c], &raw, 4);
      }
      verts += std::format(" [{:.3f} {:.3f} {:.3f} {:.3f}]", f[0], f[1], f[2], f[3]);
    }
    EOT_INFO("[diag]   rect verts (next draw){}", verts);
  }
  mark(9);
  // fix117: what this quad list derived, for the next one with the same window
  if (rp_eligible) {
    const bool cacheable = defer_this && ps && pk.targets.colorCount &&
                           (!instancing_on || InstanceStreamMask(*layout, vs->hash) == 0);
    if (rp_predicted && strip_mode >= 3) {
      u32 differs = cacheable ? 0u : 256u;
      differs |= pk.vs != rp.vs ? 1u : 0u;
      differs |= pk.ps != rp.ps ? 2u : 0u;
      differs |= pk.layout != rp.layout ? 4u : 0u;
      {
        // (only what the count and the depth surface make meaningful: the rest of a packet's
        // TargetWords is whatever its queue slot held before)
        const TargetWords &a = pk.targets, &b = rp.targets;
        bool same = a.colorCount == b.colorCount && a.depthVa == b.depthVa;
        for (u32 i = 0; same && i < a.colorCount; ++i)
          same = a.colorVa[i] == b.colorVa[i] && a.colorInfo[i] == b.colorInfo[i] &&
                 std::memcmp(a.colorWords[i], b.colorWords[i], sizeof a.colorWords[i]) == 0;
        if (same && a.depthVa)
          same = std::memcmp(a.depthWords, b.depthWords, sizeof a.depthWords) == 0;
        differs |= same ? 0u : 8u;
      }
      differs |= std::memcmp(pk.strides, rp.strides, sizeof pk.strides) != 0 ? 16u : 0u;
      differs |= pk.max_slot != rp.max_slot ? 32u : 0u;
      differs |= need_zero != rp.need_zero ? 64u : 0u;
      for (u32 S = 0; S <= max_slot && S <= rp.max_slot; ++S) {
        const DrawPacket::DeferredStream &a = pk.dStreams[S], &b = rp.streams[S];
        if (!(a.data == b.data && a.dataVa == b.dataVa && a.stride == b.stride &&
              a.sizeBytes == b.sizeBytes && a.objectVa == b.objectVa && a.stream == b.stream))
          differs |= 128u;
      }
      if (differs && ++rp.mismatches <= 24)
        EOT_WARN("[strip-fast] a repeat would have differed (what: {:#x}): vs {:016x} ps {:016x}",
                 differs, vs->hash, ps ? ps->hash : 0ull);
    }
    rp.valid = cacheable;
    if (cacheable) {
      rp.frame = s.captured_presents;
      rp.generation = s.shader_generation.load(std::memory_order_acquire);
      rp.epoch = UploadRingEpoch();
      rp.decl = dev.U32(dev::kVertexDeclaration);
      StripBlocksSave(pk.window.image, rp.blocks);
      rp.vs = vs;
      rp.ps = ps;
      rp.vs_va = vs_va;
      rp.ps_va = ps_va;
      rp.layout = layout;
      rp.targets = pk.targets;
      std::memcpy(rp.strides, pk.strides, sizeof rp.strides);
      rp.max_slot = max_slot;
      rp.need_zero = need_zero;
      for (u32 S = 0; S <= max_slot; ++S)
        rp.streams[S] = pk.dStreams[S];
    }
  }
  return true;
}

struct SurfaceSignature {
  const void *host = nullptr, *single = nullptr, *redirect = nullptr, *redirectHost = nullptr;
  u32 samples = 0;
  i32 redirectX = 0, redirectY = 0;
  u8 content = 0;
  bool contentInSingle = false, imagesAgree = false, resolvedSinceDraw = false;
  bool singleDirty = false, singleStale = false;
  bool operator==(const SurfaceSignature &o) const {
    return host == o.host && single == o.single && redirect == o.redirect &&
           redirectHost == o.redirectHost && samples == o.samples && redirectX == o.redirectX &&
           redirectY == o.redirectY && content == o.content &&
           contentInSingle == o.contentInSingle && imagesAgree == o.imagesAgree &&
           resolvedSinceDraw == o.resolvedSinceDraw && singleDirty == o.singleDirty &&
           singleStale == o.singleStale;
  }
};

SurfaceSignature SignSurface(const GuestSurface &surf) {
  SurfaceSignature sig{};
  sig.host = surf.host.texture.get();
  sig.single = surf.single.texture.get();
  sig.redirect = surf.redirectMirror.get();
  sig.redirectHost = surf.redirectMirror ? surf.redirectMirror->host.texture.get() : nullptr;
  sig.samples = surf.host.sampleCount;
  sig.redirectX = surf.redirectX;
  sig.redirectY = surf.redirectY;
  sig.content = static_cast<u8>(surf.content);
  sig.contentInSingle = surf.contentInSingle;
  sig.imagesAgree = surf.imagesAgree;
  sig.resolvedSinceDraw = surf.resolvedSinceDraw;
  sig.singleDirty = surf.singleDirty;
  sig.singleStale = surf.singleSerial != surf.writeSerial;
  return sig;
}

// WoS port (fix107): wos_skip_shaders - draws whose vs or ps hash starts with one of the listed
// hex prefixes are not drawn, to find in-game which draw paints a broken shape
namespace {
struct SkipPrefix {
  u64 value, mask;
};
const std::vector<SkipPrefix> &SkipList() {
  static const std::vector<SkipPrefix> list = [] {
    std::vector<SkipPrefix> out;
    const std::string v = wos_skip_shaders() ? wos_skip_shaders() : "";
    size_t i = 0;
    while (i < v.size()) {
      size_t j = v.find_first_of(", ;", i);
      if (j == std::string::npos)
        j = v.size();
      std::string t = v.substr(i, j - i);
      if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
        t = t.substr(2);
      if (!t.empty() && t.size() <= 16 &&
          t.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
        const u32 bits = static_cast<u32>(t.size()) * 4;
        const u64 val = std::stoull(t, nullptr, 16) << (64 - bits);
        const u64 mask = bits >= 64 ? ~0ull : ~(~0ull >> bits);
        out.push_back({val, mask});
        EOT_INFO("[skip] fix107: draws with shader {} are skipped", t);
      }
      i = j + 1;
    }
    return out;
  }();
  return list;
}
bool SkipByShader(u64 vs, u64 ps) {
  const auto &l = SkipList();
  if (l.empty())
    return false;
  for (const auto &p : l)
    if ((vs & p.mask) == p.value || (ps & p.mask) == p.value)
      return true;
  return false;
}
}  // namespace

void ReplayDraw(VideoState &s, const DrawPacket &pk) {
  static u32 replay_count = 0;
  const bool timed = (++replay_count & 15u) == 0;
  u64 lap_t0 = timed ? PerfNow() : 0;
  auto lap = [&](f64 &acc) {
    if (!timed)
      return;
    const u64 t1 = PerfNow();
    acc += 16.0 * static_cast<f64>(t1 - lap_t0) * PerfMsPerTick();
    lap_t0 = t1;
  };
  DeviceView dev = Device(pk.device_va);
  dev.snapshot = pk.window.image;
  dev.snapshotSize = kDeviceSnapshotBytes;
  dev.pages = DeviceWindow::PageTable();
  GuestShader *vs = pk.vs;
  GuestShader *ps = pk.ps;
  const InputLayout *layout = pk.layout;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  constexpr u32 kSharedTextureBytes = offsetof(SharedConstants, booleans);
  static_assert(kSharedTextureBytes == 20 * 16);
  struct ReplayMemo {
    bool valid = false;
    GuestShader *vs = nullptr, *ps = nullptr;
    const InputLayout *layout = nullptr;
    const HostTexture *images[5] = {};
    u32 colorCount = 0, samples = 0;
    i32 offsetX = 0, offsetY = 0;
    plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    bool rectList = false;
    u32 strides[16] = {};
    u8 state[DeviceWindow::kBlocks[2].size];
    u8 consts[DeviceWindow::kBlocks[1].size];
    PipelineState st;
    u32 spec = 0;
    bool a2c = false;
    plume::RenderPipeline *pipeline = nullptr;
    ViewportInfo vp;
    u32 jitterIndex = 0;
    velocity::Target *velocity = nullptr;
    SharedConstants fixed;
    UploadAlloc shared;
    u64 sharedRing = ~0ull;
    u8 sharedTextures[kSharedTextureBytes] = {};
    u32 sharedSint = 0, sharedBiased = 0;
  };
  constexpr u32 kReplayMemoWays = 8;
  static ReplayMemo memos[kReplayMemoWays];
  static u32 memo_next = 0;
  const u8 *state_block = pk.window.image + DeviceWindow::kBlocks[2].base;
  const u8 *consts_block = pk.window.image + DeviceWindow::kBlocks[1].base;
  const u32 jitter_index = taa::JitterIndex(s, pk.taaSkip, pk.rectList);
  float jitter_x = 0.0f, jitter_y = 0.0f;
  if (!pk.rectList)
    taa::FrameJitter(s, pk.taaSkip, pk.rectList, &jitter_x, &jitter_y);
  ReplayMemo *memo = nullptr;
  for (ReplayMemo &m : memos) {
    if (m.valid && m.vs == vs && m.ps == ps && m.layout == layout && m.topology == pk.topology &&
        m.jitterIndex == jitter_index &&
        m.rectList == pk.rectList && std::memcmp(m.strides, pk.strides, sizeof(pk.strides)) == 0 &&
        std::memcmp(m.state, state_block, sizeof(m.state)) == 0 &&
        std::memcmp(m.consts, consts_block, sizeof(m.consts)) == 0) {
      memo = &m;
      break;
    }
  }
  const bool state_hit = memo != nullptr;

  struct ClsKey {
    u32 dc = 0, srm = 0, cc = 0, bc = 0;
    bool hasPs = false, rect = false;
    bool operator==(const ClsKey &o) const {
      return dc == o.dc && srm == o.srm && cc == o.cc && bc == o.bc && hasPs == o.hasPs &&
             rect == o.rect;
    }
  };
  const ClsKey cls_key{dev.U32(dev::kDepthControl), dev.U32(dev::kStencilRefMask),
                       dev.U32(dev::kColorControl), dev.U32(dev::kBlendControl0), ps != nullptr,
                       pk.rectList};
  struct TargetMemo {
    bool valid = false;
    ClsKey key;
    TargetWords words;
    u64 surfaceGeneration = 0;
    SurfaceSignature sigs[5];
    Targets targets;
    DrawClass cls;
  };
  static TargetMemo tmemo;
  Targets targets;
  DrawClass cls;
  bool target_hit = tmemo.valid && tmemo.key == cls_key &&
                    tmemo.surfaceGeneration == s.surface_generation &&
                    std::memcmp(&tmemo.words, &pk.targets, sizeof(TargetWords)) == 0;
  if (target_hit) {
    const Targets &t = tmemo.targets;
    for (u32 i = 0; target_hit && i < t.colorCount; ++i) {
      const SurfaceSignature sig = SignSurface(*t.color[i]);
      target_hit = sig == tmemo.sigs[i];
    }
    if (target_hit && t.depth) {
      const SurfaceSignature sig = SignSurface(*t.depth);
      target_hit = sig == tmemo.sigs[4];
    }
  }
  if (target_hit) {
    targets = tmemo.targets;
    cls = tmemo.cls;
    s.perf.target_memo_hits++;
  } else {
    if (tmemo.valid) {
      if (tmemo.surfaceGeneration != s.surface_generation)
        s.perf.target_memo_miss_gen++;
      else if (!(tmemo.key == cls_key) ||
               std::memcmp(&tmemo.words, &pk.targets, sizeof(TargetWords)) != 0)
        s.perf.target_memo_miss_words++;
      else {
        s.perf.target_memo_miss_sig++;
        static u32 logged = 0;
        if (logged < 12) {
          const Targets &t = tmemo.targets;
          for (u32 i = 0; i < 5; ++i) {
            const GuestSurface *surf = i < 4 ? (i < t.colorCount ? t.color[i] : nullptr) : t.depth;
            if (!surf)
              continue;
            const SurfaceSignature now = SignSurface(*surf), was = tmemo.sigs[i];
            if (now == was)
              continue;
            ++logged;
            EOT_DEBUG("[draw] target memo miss: surface {} ({:#x}) host {}/{} single {}/{} redirect {}/{} "
                      "samples {}/{} content {}/{} cis {}/{} agree {}/{} rsd {}/{} dirty {}/{} stale {}/{}",
                      i, surf->va, was.host, now.host, was.single, now.single, was.redirect,
                      now.redirect, was.samples, now.samples, was.content, now.content,
                      was.contentInSingle, now.contentInSingle, was.imagesAgree, now.imagesAgree,
                      was.resolvedSinceDraw, now.resolvedSinceDraw, was.singleDirty, now.singleDirty,
                      was.singleStale, now.singleStale);
          }
        }
      }
    }
    tmemo.valid = false;
    if (!ResolveTargetsFromWords(s, pk.targets, targets)) {
      Dropped("no render target or depth surface bound", 0x6003);
      return;
    }
    ghost::NoteDraw(s.guest_frames, targets);
    cls = ClassifyDraw(dev, targets, ps != nullptr, pk.rectList);
    if (cls.nullPs && !cls.depthWrite && !cls.stencil) {
      if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()))
        EOT_INFO("[diag] skip prim {} n={} dc={:#x} rt0={:#x} ds={:#x} vs={:016x}: no pixel shader, no "
                 "depth or stencil write",
                 pk.prim, pk.indexed ? pk.index_count : pk.vertexCount, dev.U32(dev::kDepthControl),
                 targets.colorCount ? targets.color[0]->va : 0, targets.depth ? targets.depth->va : 0,
                 vs->hash);
      s.perf.draws--;
      s.perf.draws_skipped++;
      return;
    }
    SurfaceSignature sigs[5];
    for (u32 i = 0; i < targets.colorCount; ++i)
      sigs[i] = SignSurface(*targets.color[i]);
    if (targets.depth)
      sigs[4] = SignSurface(*targets.depth);
    if (!SelectTargetImages(s, targets, cls)) {
      Dropped("surface image unavailable", 0x6016);
      return;
    }
    tmemo.key = cls_key;
    tmemo.words = pk.targets;
    tmemo.surfaceGeneration = s.surface_generation;
    std::memcpy(tmemo.sigs, sigs, sizeof(sigs));
    tmemo.targets = targets;
    tmemo.cls = cls;
    tmemo.valid = true;
  }
  lap(s.perf.replay_targets_ms);

  if (pk.velocity && Settings::MotionVectors() && targets.colorCount == 1 && targets.depth &&
      targets.colorImage[0] && !cls.rect)
    targets.velocity = velocity::CurrentFor(s, *targets.depth, targets.colorImage[0]->width,
                                            targets.colorImage[0]->height, targets.samples);

  bool memo_hit = state_hit && memo->colorCount == targets.colorCount &&
                  memo->samples == targets.samples && memo->offsetX == targets.offsetX &&
                  memo->offsetY == targets.offsetY && memo->images[4] == targets.depthImage &&
                  memo->velocity == targets.velocity;
  for (u32 i = 0; memo_hit && i < targets.colorCount; ++i)
    memo_hit = memo->images[i] == targets.colorImage[i];

  PipelineState st;
  u32 spec = 0;
  bool a2c = false;
  plume::RenderPipeline *pipeline = nullptr;
  ViewportInfo vp;
  static SharedConstants sc;
  if (memo_hit) {
    st = memo->st;
    spec = memo->spec;
    a2c = memo->a2c;
    pipeline = memo->pipeline;
    vp = memo->vp;
    sc = memo->fixed;
    s.perf.replay_memo_hits++;
  } else {
    sc.biasedTextures = 0;
    ZeroPipelineState(st);
    spec = layout->spec;
    FillPipelineState(dev, targets, st, &spec, &a2c);
    // WoS port (fix53/54): the city's medium-distance buildings are drawn as a depth-only
    // shell first (no pixel shader, no colour target, main depth, GEQUAL) and later for real.
    // fix53 pushed the shells 32 depth steps back - the buildings stayed black. fix54 does
    // not draw them: they only occlude (early-z), the GPU has room to spare (~1.7 ms a
    // frame), and no main-pass draw tests EQUAL against them. If the buildings now show,
    // the shells' depth hid them; if the street/sky shows through, the buildings are not
    // drawn there at all.
    // fix60: the black buildings were the facade LOD's integer positions (fix58), not the
    // shells - they are drawn again as on the console (early-z, LOD occlusion) unless
    // wos_recomp.toml sets wos_depth_shells = false.
    if (!ps && targets.depth && targets.colorCount == 0 && st.depthWrite &&
        !wos_depth_shells() &&
        !(targets.depth->isDepth && targets.depth->width == 1024 && targets.depth->height == 1024)) {
      static u64 skipped = 0;
      if (skipped++ == 0)
        EOT_INFO("[standin] depth-only shells into the {}x{} depth are not drawn "
                 "(wos_depth_shells = false)",
                 targets.depth->width, targets.depth->height);
      return;
    }
    spec |= layout->spec;
    const VsVariant variant = VsVariantFor(vs->entry, ps ? ps->entry : nullptr, ps == nullptr,
                                           targets.velocity != nullptr);
    if (targets.velocity && variant != VsVariant::Velocity)
      targets.velocity = nullptr;
    st.vs = ResolveHostShader(s, *vs, spec, variant);
    st.ps = ps ? ResolveHostShader(s, *ps, spec,
                                   variant == VsVariant::Velocity ? VsVariant::Velocity
                                                                  : VsVariant::Trimmed)
               : nullptr;
    if (targets.velocity) {
      const u32 v = st.rtCount;
      st.rtFormats[v] = velocity::kFormat;
      st.blend[v] = plume::RenderBlendDesc::Copy();
      st.rtCount = v + 1;
      st.velocity = true;
      static u64 logged_frame = ~0ull;
      if (Settings::DiagVerbosity() >= 2 && logged_frame != s.guest_frames / 600) {
        logged_frame = s.guest_frames / 600;
        EOT_DEBUG("[velocity] draw vs {:016x} ps {:016x} variant {} rt0 fmt {} blend {} mask {:#x} "
                  "rt1 fmt {} samples {} depth write {} spec {:#x}",
                  vs->hash, ps ? ps->hash : 0ull, static_cast<u32>(variant),
                  static_cast<u32>(st.rtFormats[0]), st.blend[0].blendEnabled,
                  st.blend[0].renderTargetWriteMask, static_cast<u32>(st.rtFormats[1]),
                  st.sampleCount, st.depthWrite, spec);
      }
    }
    if (!st.vs || (ps && !st.ps)) {
      Dropped("host shader unavailable (link failure)", 0x6007);
      return;
    }
    st.layout = layout;
    st.vsHash = vs->hash;
    st.psHash = ps ? ps->hash : 0;
    // WoS port (fix84): diagnostic - wos_debug_skip_shadow_casters leaves the shadow maps
    // (1024x1024 depth-only passes) empty: nothing casts a shadow, everything is lit. Unlike
    // wos_debug_no_shadow_maps (an unbound map may read as "all in shadow") this is a clean
    // "are the black road blocks a shadow" test.
    {
      static const bool skip_casters = wos_debug_skip_shadow_casters();
      if (skip_casters && !targets.colorCount && targets.depth && targets.depth->width == 1024 &&
          targets.depth->height == 1024)
        return;
    }
    if (!targets.colorCount && targets.depth) {
      struct Snap {
        u32 mode, cache204, cache208, vtx, vte, win;
        float fs, fo, bs, bo, xs, xo, ys, yo;
      };
      static Snap last{};
      static u32 lines = 0;
      const Snap now{dev.U32(dev::kModeControl) & 0x1800u, 0u /* WoS port: EoT-only global */,
                     0u, dev.U32(dev::kVtxControl),
                     dev.U32(dev::kVteControl), dev.U32(dev::kWindowOffset),
                     dev.F32(dev::kPolyOffsetFrontScale), dev.F32(dev::kPolyOffsetFrontOffset),
                     dev.F32(dev::kPolyOffsetBackScale), dev.F32(dev::kPolyOffsetBackOffset),
                     dev.F32(dev::kVportXScale), dev.F32(dev::kVportXOffset),
                     dev.F32(dev::kVportYScale), dev.F32(dev::kVportYOffset)};
      if (std::memcmp(&now, &last, sizeof(now)) != 0 && lines < 64) {
        last = now;
        ++lines;
        EOT_DEBUG("[shadow] caster draw: mode bits {:#x} front scale {:g} offset {:g} back scale {:g} "
                 "offset {:g} | cache 204 {:g} 208 {:g} -> host bias {} slope {:g} (vs {:016x}) | "
                 "vport x {:g}/{:g} y {:g}/{:g} vte {:#x} vtx {:#x} win {:#x} scale {:.4f}",
                 now.mode, now.fs, now.fo, now.bs, now.bo, std::bit_cast<float>(now.cache204),
                 std::bit_cast<float>(now.cache208), st.depthBias,
                 st.slopeScaledDepthBias * st.targetScale, st.vsHash, now.xs, now.xo, now.ys,
                 now.yo, now.vte, now.vtx, now.win, targets.scale);
      }
    }
    st.layoutKey = layout->key;
    st.spec = spec;
    for (u32 S = 0; S < 16; ++S)
      st.strides[S] = pk.strides[S];
    st.topology = pk.topology;
    if (pk.rectList)
      st.cull = plume::RenderCullMode::NONE;
    CanonicalizePipelineState(st,
                              (vs->entry ? vs->entry->specConstantsMask : 0u) |
                                  (ps && ps->entry ? ps->entry->specConstantsMask : 0u),
                              layout->streamMask);
    static PipelineState last_state{};
    static plume::RenderPipeline *last_pipeline = nullptr;
    pipeline = last_pipeline;
    if (!pipeline || std::memcmp(reinterpret_cast<const u8 *>(&st) + kPipelineKeyOffset,
                                 reinterpret_cast<const u8 *>(&last_state) + kPipelineKeyOffset,
                                 sizeof(st) - kPipelineKeyOffset) != 0) {
      pipeline = GetOrCreatePipeline(s, st);
      if (pipeline) {
        last_state = st;
        last_pipeline = pipeline;
      }
    }
    if (!pipeline) {
      Dropped("pipeline creation failed", 0x6008);
      return;
    }

    vp = ComputeViewport(dev, targets, jitter_x, jitter_y);
    for (u32 i = 0; i < 4; ++i) {
      sc.booleans[i] = dev.U32(dev::kVsBoolConstants + 4 * i);
      sc.booleans[4 + i] = dev.U32(dev::kPsBoolConstants + 4 * i);
    }
    FillLoopConstants(dev, dev::kVsLoopConstants, &sc.loopConstants[0]);
    FillLoopConstants(dev, dev::kPsLoopConstants, &sc.loopConstants[16]);
    sc.swappedTexcoords = layout->swappedTexcoords;
    sc.swappedNormals = layout->swappedNormals;
    sc.swappedBinormals = layout->swappedBinormals;
    sc.swappedTangents = layout->swappedTangents;
    sc.swappedBlendWeights = layout->swappedBlendWeights;
    sc.swappedPositions = layout->swappedPositions;
    sc.sintTexcoords = layout->sintTexcoords;
    sc.packedDec3 = layout->packedDec3;
    if (targets.colorCount) {
      const u32 f0 = targets.color[0]->colorFormat;
      if (f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA))
        sc.packedDec3 |= 1u << 31;
      if (f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_8_8_8_8) ||
          f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) ||
          f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_2_10_10_10) ||
          f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10))
        sc.packedDec3 |= 1u << 30;
    }
    sc.alphaThreshold = dev.F32(dev::kAlphaRef);
    std::memcpy(sc.posScale, vp.posScale, sizeof(sc.posScale));
    std::memcpy(sc.posOffset, vp.posOffset, sizeof(sc.posOffset));

    ReplayMemo &fill = memo ? *memo : memos[memo_next++ % kReplayMemoWays];
    fill.valid = true;
    fill.vs = vs;
    fill.ps = ps;
    fill.layout = layout;
    fill.jitterIndex = jitter_index;
    fill.velocity = targets.velocity;
    for (u32 i = 0; i < 4; ++i)
      fill.images[i] = i < targets.colorCount ? targets.colorImage[i] : nullptr;
    fill.images[4] = targets.depthImage;
    fill.colorCount = targets.colorCount;
    fill.samples = targets.samples;
    fill.offsetX = targets.offsetX;
    fill.offsetY = targets.offsetY;
    fill.topology = pk.topology;
    fill.rectList = pk.rectList;
    std::memcpy(fill.strides, pk.strides, sizeof(fill.strides));
    std::memcpy(fill.state, state_block, sizeof(fill.state));
    std::memcpy(fill.consts, consts_block, sizeof(fill.consts));
    fill.st = st;
    fill.spec = spec;
    fill.a2c = a2c;
    fill.pipeline = pipeline;
    fill.vp = vp;
    fill.fixed = sc;
    fill.sharedRing = ~0ull;
    memo = &fill;
  }
  s.current_vs_va = pk.vs_va;
  s.current_ps_va = pk.ps_va;
  {
    static const char *const kOrigins[2][3] = {{"stream/none", "stream/stream", "stream/bundle"},
                                               {"bundle/none", "bundle/stream", "bundle/bundle"}};
    s.current_origin =
        kOrigins[vs->createdByGuestCall ? 1 : 0][ps ? (ps->createdByGuestCall ? 2 : 1) : 0];
  }
  lap(s.perf.pso_lookup_ms);

  {
    PerfScope bind_scope(s.perf.bind_ms);
    u32 texture_mask = vs->textureFetchMask | (ps ? ps->textureFetchMask : 0u);
    // WoS port (fix74): the city facade glass's city-reflection cube (slot 3) flashes and
    // speckles on the PS5 (fix69-73: inputs, constants, cube content and LOD all verified;
    // the lookup direction jumps between cube faces). Until that is found, the facade
    // shaders sample it as black unless wos_facade_reflections = true.
    // fix75: not unbound (a black cube) - some facades then vanish at mid distance (their
    // output alpha depends on it); the sky-light cube of slot 0 stands in instead (below)
    bool facade_glass = false;
    if (ps) {
      static const bool reflections = wos_facade_reflections();
      const u64 h = ps->hash;
      facade_glass = !reflections && (h == 0x5acb3796e01eed3cull || h == 0x5f18a4e3ead2ec0cull ||
                                      h == 0x6075f07454832b24ull || h == 0xc447a6667bf82a6dull);
    }
    const u32 dc = dev.U32(dev::kDepthControl);
    const u32 zfunc = (dc >> 4) & 7;
    const bool scene_draw = !pk.rectList && (dc & 2) &&
                            (zfunc == 1 || zfunc == 3 || zfunc == 4 || zfunc == 6);
    BindTexturesAndSamplers(s, dev, texture_mask, sc, scene_draw ? &targets : nullptr);
    if (facade_glass && sc.textureCubeIndices[0] != kNullTextureCubeDescriptorIndex) {
      sc.textureCubeIndices[3] = sc.textureCubeIndices[0];
      sc.samplerIndices[3] = sc.samplerIndices[0];
    }
  }
  if (pk.hasCameraVP)
    taa::BeforeSceneConsumerDraw(s, s.draw_bound_textures, pk.cameraVP, ps ? ps->hash : 0, pk.taaSkip);
  UploadAlloc shared_alloc;
  const u64 ring_epoch = UploadRingEpoch();
  if (memo->sharedRing == ring_epoch &&
      std::memcmp(memo->sharedTextures, &sc, kSharedTextureBytes) == 0 &&
      memo->sharedSint == sc.sintTexcoords && memo->sharedBiased == sc.biasedTextures) {
    shared_alloc = memo->shared;
  } else {
    if (!UploadBytes(&sc, sizeof(sc), kConstantBufferAlignment, &shared_alloc)) {
      Dropped("shared constant upload failed", 0x6011);
      return;
    }
    s.perf.constant_bytes += sizeof(SharedConstants);
    memo->shared = shared_alloc;
    memo->sharedRing = ring_epoch;
    std::memcpy(memo->sharedTextures, &sc, kSharedTextureBytes);
    memo->sharedSint = sc.sintTexcoords;
    memo->sharedBiased = sc.biasedTextures;
  }
  lap(s.perf.const_ms);

  // WoS port: always on, once: 24 scene draws after guest frame 3000
    {
      // WoS port: the vertex side of the first draws of a diagnostic frame - the VS float
      // constants c0..c7 the draw was captured with and stream 0's first vertices (big-endian
      // words, from guest memory now), to tell missing matrices from missing vertex data
      static u32 vdiag = 0;
      static i32 vdiag_for = 0;  // WoS port (fix34): again for every armed dump frame
      if (vdiag_for != Settings::DumpFrame()) {
        vdiag_for = Settings::DumpFrame();
        vdiag = 0;
      }
      if (vdiag < 24 && targets.depth && targets.colorCount && ps && targets.width >= 1280 &&
          s.guest_frames + 1 >= static_cast<u64>(Settings::DumpFrame())) {
        ++vdiag;
        // the float file this draw was actually given (host order), first 32 registers
        std::string cs;
        if (const auto *cf = reinterpret_cast<const float *>(pk.vs_consts.cpu)) {
          const u32 n = std::min<u32>(32, static_cast<u32>(pk.vs_consts.size / 16));
          for (u32 i = 0; i < n; ++i)
            cs += std::format(" c{}=({:.4g},{:.4g},{:.4g},{:.4g})", i, cf[4 * i], cf[4 * i + 1],
                              cf[4 * i + 2], cf[4 * i + 3]);
        }
        EOT_INFO("[diag]   vs regs={} inputs={} file{}", vs->floatConstantRegs,
                 layout ? layout->streamMask : 0u, cs);
        const u32 decl = dev.U32(dev::kVertexDeclaration);
        std::string ds;
        if (const u8 *d = decl ? mem::at<const u8>(decl) : nullptr) {
          const u32 count = std::min<u32>(8, rex::memory::load_and_swap<u32>(d + obj::kDeclElementCount));
          for (u32 i = 0; i < count; ++i) {
            const u8 *e = d + obj::kDeclElements + 12 * i;
            ds += std::format(" [s{} +{} type {:#x} usage {}.{}]",
                              rex::memory::load_and_swap<u16>(e),
                              rex::memory::load_and_swap<u16>(e + 2),
                              rex::memory::load_and_swap<u32>(e + 4), e[9], e[10]);
          }
        }
        EOT_INFO("[diag]   decl {:#x}:{}", decl, ds);
        std::string ps_cs;
        if (const auto *cf = reinterpret_cast<const float *>(pk.ps_consts.cpu)) {
          const u32 n = std::min<u32>(12, static_cast<u32>(pk.ps_consts.size / 16));
          for (u32 i = 0; i < n; ++i)
            ps_cs += std::format(" c{}=({:.4g},{:.4g},{:.4g},{:.4g})", i, cf[4 * i], cf[4 * i + 1],
                                 cf[4 * i + 2], cf[4 * i + 3]);
        }
        std::string tx;
        const u32 tmask = vs->textureFetchMask | ps->textureFetchMask;
        tx += std::format(" texmask={:#x}", tmask);
        for (u32 t = 0; t < 16; ++t) {
          const u32 w0 = dev.U32(dev::kFetchConstants + 24 * t);
          const u32 w1 = dev.U32(dev::kFetchConstants + 24 * t + 4);
          const u32 w2 = dev.U32(dev::kFetchConstants + 24 * t + 8);
          if (!(tmask & (1u << t)) && !dev.U32(dev::kTextureObject0 + 4 * t))
            continue;
          // the first 16 bytes the texture's base address holds now
          u32 sample[4] = {};
          const u32 base = w1 & 0xFFFFF000u;
          if (const u8 *p = base ? mem::gpu<const u8>(base < 0x20000000u ? base : base) : nullptr)
            for (u32 k = 0; k < 4; ++k)
              sample[k] = rex::memory::load_and_swap<u32>(p + 4 * k);
          tx += std::format(" t{}[obj {:#x} fc {:08x} {:08x} {:08x} type{} fmt{} {}x{} data {:08x}{:08x}{:08x}{:08x}]",
                            t, dev.U32(dev::kTextureObject0 + 4 * t), w0, w1, w2, w0 & 3,
                            w1 & 0x3F, (w2 & 0x1FFF) + 1, ((w2 >> 13) & 0x1FFF) + 1, sample[0],
                            sample[1], sample[2], sample[3]);
        }
        EOT_INFO("[diag]   ps {:016x} regs={} blend0={:#010x} colorctl={:#010x} colormask={:#x} "
                 "rt fmt{} exp{} |{} | file{}",
                 ps->hash, ps->floatConstantRegs, dev.U32(dev::kBlendControl0),
                 dev.U32(dev::kColorControl), dev.U32(dev::kColorMask),
                 targets.color[0]->colorFormat, targets.color[0]->colorExpBias, tx, ps_cs);
        const u32 slot0 = dev::StreamFetchSlotOffset(0);
        const u32 d0 = dev.U32(slot0), d1 = dev.U32(slot0 + 4);
        std::string vs_words;
        if (const u8 *v = (d0 & 3) == 3 ? mem::gpu<const u8>(d0 & ~3u) : nullptr) {
          const u32 stride = pk.strides[0] ? pk.strides[0] : 16;
          for (u32 k = 0; k < 3; ++k) {
            vs_words += " |";
            for (u32 w = 0; w < 4 && w * 4 < stride; ++w) {
              const u32 word = rex::memory::load_and_swap<u32>(v + k * stride + 4 * w);
              float f;
              std::memcpy(&f, &word, 4);
              vs_words += std::format(" {:08x}({:.3g})", word, f);
            }
          }
        }
        EOT_INFO("[diag]   stream0 fetch=[{:08x} {:08x}] decl={:#x}{}", d0, d1,
                 dev.U32(dev::kVertexDeclaration), vs_words);
      }
    }
  const bool diag_now =
      Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame());
  if (diag_now && Settings::Record()) {
    GpuTimingDiagMark(s, s.command_list,
                      std::format("draw rt0={:#x} {}x{} f{} smp{}{} ps={:016x} vs={:016x}",
                                  targets.colorCount ? targets.color[0]->va : 0, targets.width,
                                  targets.height,
                                  targets.colorCount ? targets.color[0]->colorFormat : 99,
                                  targets.samples,
                                  targets.depth && targets.depthImage == &targets.depth->single
                                      ? " twin"
                                      : "",
                                  ps ? ps->hash : 0, vs->hash));
  } else if (diag_now) {
    static u32 k = 0;
    const u32 vte = dev.U32(dev::kVteControl);
    GpuTimingDiagMark(s, s.command_list, std::format("draw {}", k));
    EOT_INFO("[diag] draw {} prim {} {}{} smp={}{}{}{} n={} pso={} key={:016x} mode={:#x} dc={:#x} srm={:#x} srmbf={:#x} rt0={:#x} {}x{} fmt{} rtexp{} ds={:#x} vs={:016x} ps={:016x} "
             "vp=({:.0f},{:.0f} {:.0f}x{:.0f} z{:.2f}-{:.2f}) vte={:#x} xs={:.1f} ys={:.1f} zs={:.3f} "
             "zo={:.3f} scis=({},{},{},{}) cull={} z={}{} func{} blend={} mask={:#x} spec={:#x} "
             "streams={:#x} stride0={} tex0={:#x} posScale=({:.3f},{:.3f},{:.3f}) "
             "posOff=({:.3f},{:.3f},{:.3f})",
             k++, pk.prim, pk.indexed ? "idx" : "vtx", pk.rectList ? " rect" : "", targets.samples,
             cls.additive ? " add" : "",
             targets.depth && targets.depthImage == &targets.depth->single ? " dtwin" : "",
             targets.offsetX || targets.offsetY ? " atlas" : "",
             pk.indexed ? pk.index_count : pk.vertexCount, static_cast<const void *>(pipeline),
             HashPipelineState(st), dev.U32(dev::kModeControl), dev.U32(dev::kDepthControl),
             dev.U32(dev::kStencilRefMask), dev.U32(dev::kStencilRefMaskBF),
             targets.colorCount ? targets.color[0]->va : 0, targets.width, targets.height,
             targets.colorCount ? targets.color[0]->colorFormat : 99,
             targets.colorCount ? targets.color[0]->colorExpBias : 0,
             targets.depth ? targets.depth->va : 0, vs->hash, ps ? ps->hash : 0, vp.vp.x, vp.vp.y,
             vp.vp.width, vp.vp.height, vp.vp.minDepth, vp.vp.maxDepth, vte,
             dev.F32(dev::kVportXScale), dev.F32(dev::kVportYScale), dev.F32(dev::kVportZScale),
             dev.F32(dev::kVportZOffset), vp.scissor.left, vp.scissor.top, vp.scissor.right,
             vp.scissor.bottom, static_cast<u32>(st.cull), st.depthEnable ? "on" : "off",
             st.depthWrite ? "w" : "", static_cast<u32>(st.depthFunc),
             st.blend[0].blendEnabled, st.blend[0].renderTargetWriteMask, spec,
             layout->streamMask, pk.strides[0], dev.U32(dev::kTextureObject0),
             vp.posScale[0], vp.posScale[1], vp.posScale[2], vp.posOffset[0], vp.posOffset[1],
             vp.posOffset[2]);
    for (u32 slot = 0; slot < 16 && !Settings::Record(); ++slot) {
      const u32 tex_va = dev.U32(dev::kTextureObject0 + 4 * slot);
      if (!tex_va)
        continue;
      u32 fc[6];
      for (u32 d = 0; d < 6; ++d)
        fc[d] = dev.U32(dev::kFetchConstants + 24 * slot + 4 * d);
      GuestTexture *gt = GetGuestTexture(s, tex_va, true, fc);
      EOT_INFO("[diag]   tex{} va={:#x} fc=[{:08x} {:08x} {:08x} {:08x} {:08x} {:08x}] fmt={} "
               "dim={} {}x{}x{} {} {} {} biased={:#x} base={:#x} pitch={}",
               slot, tex_va, fc[0], fc[1], fc[2], fc[3], fc[4], fc[5],
               gt ? static_cast<u32>(gt->format) : 0xFFFFu,
               gt ? static_cast<u32>(gt->dimension) : 9u, gt ? gt->width : 0u,
               gt ? gt->height : 0u, gt ? gt->depth : 0u, gt && gt->tiled ? "tiled" : "linear",
               gt && gt->gammaSigned ? "gamma" : "lin",
               gt && gt->resolveOwned ? "resolve" : "upload", sc.biasedTextures,
               gt ? gt->baseAddress : 0u, ((fc[0] >> 22) & 0x1FF) * 32);
      static u32 dumped = 0;
      if (gt && !gt->tiled && gt->format == xe::TextureFormat::k_8 && dumped < 8) {
        const u32 pitch = ((fc[0] >> 22) & 0x1FF) * 32;
        const u8 *src = mem::phys<const u8>(gt->baseAddress);
        if (src && pitch >= gt->width) {
          const std::string path =
              std::format("logs/f{}_tex{}_{:x}_{}x{}_p{}.bin", s.guest_frames + 1, slot, tex_va,
                          gt->width, gt->height, pitch);
          if (FILE *f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(src, 1, static_cast<size_t>(pitch) * gt->height, f);
            std::fclose(f);
            ++dumped;
            EOT_INFO("[diag]   -> wrote {}", path);
          }
        }
      }
    }
  }

  if (!BindTargets(s, targets)) {
    Dropped("framebuffer creation failed", 0x6012);
    return;
  }
  GuestSurface *work_surface = targets.colorCount ? targets.color[0] : targets.depth;
  if (work_surface)
    work_surface->perfDraws++;
  GpuTimingCountDraw(s);
  if (pk.max_slot == 0 && !layout->needsSyntheticSlot)
    s.perf.single_stream_draws++;
  auto *cmd = s.command_list;
  bool dynamic_state_invalid = s.bound_pipeline == nullptr;
  const bool pipeline_changed = s.bound_pipeline != pipeline;
  if (pipeline_changed) {
    cmd->setPipeline(pipeline);
    s.perf.pipeline_bind_calls++;
    if (memo_hit) {
      s.perf.pipeline_bind_on_hit++;
      static u32 logged = 0;
      if (logged++ < 8)
        EOT_DEBUG("[draw] pipeline bind on a memo hit: memo {} bound {} entry {} vs {:016x} ps {:016x} "
                  "samples {} rt0 {:#x}",
                  static_cast<const void *>(pipeline), static_cast<const void *>(s.bound_pipeline),
                  static_cast<const void *>(memo), vs->hash, ps ? ps->hash : 0, targets.samples,
                  targets.colorCount ? targets.color[0]->va : 0);
    }
    s.bound_pipeline = pipeline;
  }
#if defined(EOT_D3D12)
  static u32 last_stencil_ref = ~0u;
  if (dynamic_state_invalid)
    last_stencil_ref = ~0u;
  if (st.stencilEnable && last_stencil_ref != st.stencilRef) {
    static_cast<plume::D3D12CommandList *>(cmd)->d3d->OMSetStencilRef(st.stencilRef);
    last_stencil_ref = st.stencilRef;
    s.perf.stencil_ref_calls++;
  }
#endif
  static plume::RenderViewport last_vp;
  static plume::RenderRect last_sc;
  if (dynamic_state_invalid || std::memcmp(&last_vp, &vp.vp, sizeof(last_vp)) != 0) {
    cmd->setViewports(&vp.vp, 1);
    last_vp = vp.vp;
    s.perf.viewport_bind_calls++;
  }
  if (dynamic_state_invalid || std::memcmp(&last_sc, &vp.scissor, sizeof(last_sc)) != 0) {
    cmd->setScissors(&vp.scissor, 1);
    last_sc = vp.scissor;
    s.perf.scissor_bind_calls++;
  }
  const UploadAlloc *roots[2] = {&pk.vs_consts, &pk.ps_consts};
  auto bind_root_cbv = [&](u32 r, const UploadAlloc &alloc) {
#if defined(EOT_D3D12)
    if (alloc.gpuVa && s.root_cbv_index[r] != ~0u) {
      static_cast<plume::D3D12CommandList *>(cmd)->d3d->SetGraphicsRootConstantBufferView(
          s.root_cbv_index[r], alloc.gpuVa);
      return;
    }
    cmd->setGraphicsRootDescriptor(plume::RenderBufferReference(alloc.buffer, alloc.offset), r);
#else
    const u64 address = alloc.gpuVa;
    cmd->setGraphicsPushConstants(kGuestPushConstantRangeIndex, &address, r * static_cast<u32>(sizeof(u64)),
                                  static_cast<u32>(sizeof(u64)));
#endif
  };
  for (u32 r = 0; r < 2; ++r) {
    if (s.bound_root_buffer[r] == roots[r]->buffer && s.bound_root_offset[r] == roots[r]->offset)
      continue;
    bind_root_cbv(r, *roots[r]);
    s.bound_root_buffer[r] = roots[r]->buffer;
    s.bound_root_offset[r] = roots[r]->offset;
  }
#if defined(EOT_D3D12)
  if (s.bound_spec != spec) {
    cmd->setGraphicsPushConstants(kSpecPushConstantRangeIndex, &spec, 0, sizeof(spec));
    s.bound_spec = spec;
  }
#endif
  if (s.bound_root_buffer[2] != shared_alloc.buffer ||
      s.bound_root_offset[2] != shared_alloc.offset) {
    bind_root_cbv(2, shared_alloc);
    s.bound_root_buffer[2] = shared_alloc.buffer;
    s.bound_root_offset[2] = shared_alloc.offset;
  }
  lap(s.perf.rec_state_ms);
  auto bind_vertex_views = [&](u32 first, const plume::RenderVertexBufferView *want_views,
                               u32 count, const plume::RenderInputSlot *want_slots) {
    s.perf.vertex_bind_requests++;
    u32 first_dirty = count, last_dirty = 0;
    for (u32 i = 0; i < count; ++i) {
      const u32 slot = first + i;
      const auto &want = want_views[i];
      const auto &input = want_slots[i];
      const auto &have = s.bound_vertex_streams[slot];
      if (!have.valid || have.buffer != want.buffer.ref || have.offset != want.buffer.offset ||
          have.size != want.size || have.stride != input.stride) {
        first_dirty = std::min(first_dirty, i);
        last_dirty = i;
      }
    }
    if (first_dirty == count)
      return;
    const u32 dirty_count = last_dirty - first_dirty + 1;
    cmd->setVertexBuffers(first + first_dirty, want_views + first_dirty, dirty_count,
                          want_slots + first_dirty);
    s.perf.vertex_bind_calls++;
    for (u32 i = first_dirty; i <= last_dirty; ++i) {
      const u32 slot = first + i;
      auto &have = s.bound_vertex_streams[slot];
      have.buffer = want_views[i].buffer.ref;
      have.offset = want_views[i].buffer.offset;
      have.size = want_views[i].size;
      have.stride = want_slots[i].stride;
      have.valid = true;
    }
  };
  bind_vertex_views(0, pk.views, pk.max_slot + 1, pk.slots);
  if (layout->needsSyntheticSlot) {
    plume::RenderVertexBufferView zv(plume::RenderBufferReference(pk.zero.buffer, pk.zero.offset),
                                     4096);
    plume::RenderInputSlot zs(kSyntheticVertexSlot, 0);
    bind_vertex_views(kSyntheticVertexSlot, &zv, 1, &zs);
  }
  lap(s.perf.rec_bind_ms);
  auto bind_index_view = [&](const plume::RenderIndexBufferView &want) {
    s.perf.index_bind_requests++;
    auto &have = s.bound_index_stream;
    if (have.valid && have.buffer == want.buffer.ref && have.offset == want.buffer.offset &&
        have.size == want.size && have.format == want.format)
      return;
    cmd->setIndexBuffer(&want);
    s.perf.index_bind_calls++;
    have.buffer = want.buffer.ref;
    have.offset = want.buffer.offset;
    have.size = want.size;
    have.format = want.format;
    have.valid = true;
  };
  const u32 index_count = pk.index_count;
  if (SkipByShader(st.vsHash, st.psHash)) {
    lap(s.perf.record_ms);
    return;
  }
  if (pk.rectList) {
    plume::RenderIndexBufferView ib(
        plume::RenderBufferReference(pk.index_alloc.buffer, pk.index_alloc.offset), index_count * 4,
        plume::RenderFormat::R32_UINT);
    bind_index_view(ib);
    cmd->drawIndexedInstanced(index_count, 1, 0, 0, 0);
  } else if (pk.hasCached) {
    const u32 elem = pk.cached.is32 ? 4 : 2;
    u64 capacity = 0;
    plume::RenderBuffer *chunk = PoolResidentBuffer(s, index_pool(), pk.cached.buffer,
                                                    pk.cached.offset, u64(index_count) * elem,
                                                    &capacity);
    const u64 view_bytes = capacity ? capacity : pk.cached.offset + u64(index_count) * elem;
    plume::RenderIndexBufferView ib(
        plume::RenderBufferReference(chunk, 0), static_cast<u32>(std::min<u64>(view_bytes, 0xFFFFFFFFu)),
        pk.cached.is32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
    bind_index_view(ib);
    cmd->drawIndexedInstanced(index_count, 1, static_cast<u32>(pk.cached.offset / elem),
                              pk.host_base_vertex, 0);
  } else if (pk.indexed) {
    plume::RenderIndexBufferView ib(
        plume::RenderBufferReference(pk.index_alloc.buffer, pk.index_alloc.offset), index_count * 4,
        plume::RenderFormat::R32_UINT);
    bind_index_view(ib);
    cmd->drawIndexedInstanced(index_count, 1, 0, pk.host_base_vertex, 0);
  } else {
    cmd->drawInstanced(pk.vertexCount, 1, static_cast<u32>(pk.host_base_vertex), 0);
  }
  lap(s.perf.record_ms);
  DrainHostDebugMessages(s, "draw");
  // WoS port: with eot_diag_dump, guest frame 3000's full-screen colour target right after
  // its 1st, 150th, 300th... colour draw - colour that appears and is lost later, or never
  if (Settings::DiagDump() && s.guest_frames + 1 == static_cast<u64>(Settings::DumpFrame()) &&
      targets.colorCount && targets.width >= 1280 && targets.colorImage[0] &&
      targets.colorImage[0]->texture) {
    static u32 n = 0, dumped = 0;
    static u64 for_frame = 0;
    if (for_frame != s.guest_frames + 1) {
      for_frame = s.guest_frames + 1;
      n = dumped = 0;
    }
    ++n;
    // fix85: every colour draw of the dump frame logged, and a dump every wos_dump_every
    // draws (up to wos_dump_max) - to find the draw that should
    // have drawn a missing or black piece of ground
    static const u32 every = static_cast<u32>(std::max(1, wos_dump_every()));
    static const u32 max_dumps = static_cast<u32>(std::max(1, wos_dump_max()));
    EOT_INFO("[mid] f{} n{} vs {:016x} ps {:016x} {} {} verts/idx, rt {:#x} {}x{} depth {:#x} write {} "
             "samples {} stencil {} zfunc {} cull {}",
             s.guest_frames + 1, n, st.vsHash, st.psHash, pk.indexed ? "indexed" : "plain",
             pk.indexed ? index_count : pk.vertexCount, targets.color[0] ? targets.color[0]->va : 0u,
             targets.width, targets.height, targets.depth ? targets.depth->va : 0u, st.depthWrite,
             targets.samples, st.stencilEnable, static_cast<u32>(st.depthFunc), static_cast<u32>(st.cull));
    if ((n == 1 || n % every == 0) && dumped < max_dumps) {
      ++dumped;
      ::mkdir("/app0/userdata/wos_recomp/dump", 0777);
      const std::string path = std::format("/app0/userdata/wos_recomp/dump/f{}_mid{:04}_rt{:x}.ppm",
                                           s.guest_frames + 1, n,
                                           targets.color[0] ? targets.color[0]->va : 0u);
      DumpHostTextureLocked(s, *targets.colorImage[0], path.c_str(), 1.0f);
    }
    // WoS port (fix89): a 1/8-size image after EVERY colour draw of the dump frame (up to 600),
    // to see which draw puts (or fails to put) what on screen - wos_dump_small = false: off
    static const bool small = wos_dump_small();
    static const int small_from = wos_dump_small_from(), small_to = wos_dump_small_to();
    if (small && static_cast<int>(n) >= small_from && static_cast<int>(n) <= small_to) {
      ::mkdir("/app0/userdata/wos_recomp/dump", 0777);
      const std::string path =
          std::format("/app0/userdata/wos_recomp/dump/f{}_d{:04}_{:016x}_{:016x}.ppm",
                      s.guest_frames + 1, n, st.vsHash, st.psHash);
      DumpHostTextureLocked(s, *targets.colorImage[0], path.c_str(), 1.0f, kInvalidDescriptorIndex,
                            kIdentityFetchSwizzle, 8);
    }
  }
  // WoS port: the read side - in guest frame 3000, what each draw that samples a
  // resolved (or would-be resolved) texture actually reads: a line per such binding
  // (first 48) and, with eot_diag_dump, each such colour texture's image once (first 16)
  if (s.guest_frames + 1 == static_cast<u64>(Settings::DumpFrame())) {
    static u32 lines = 0, dumps = 0, draw_no = 0;
    static std::vector<const GuestTexture *> dumped;
    static u64 for_frame = 0;
    if (for_frame != s.guest_frames + 1) {  // WoS port (fix34): every armed dump frame
      for_frame = s.guest_frames + 1;
      lines = dumps = draw_no = 0;
      dumped.clear();
    }
    ++draw_no;
    for (u32 slot = 0; slot < 16; ++slot) {
      GuestTexture *t = s.draw_bound_textures[slot];
      if (!t || !(t->resolveOwned || t->lastResolve || t->baseAddress >= 0x1C000000u))
        continue;
      static std::vector<std::pair<const GuestTexture *, u32>> logged;
      const u32 rt_va = targets.color[0] ? targets.color[0]->va : 0u;
      if (lines < 96 && std::find(logged.begin(), logged.end(), std::make_pair((const GuestTexture *)t, rt_va)) ==
                            logged.end()) {
        logged.emplace_back(t, rt_va);
        ++lines;
        EOT_INFO("[readside] draw {} rt {:#x} slot {} reads {:#x} {}x{} fmt {} base {:#x}: resolveOwned {} "
                 "provisional {} uploaded {} last resolved frame {} (now {}) host {}x{} fmt {}",
                 draw_no, targets.color[0] ? targets.color[0]->va : 0u, slot, t->va, t->width,
                 t->height, static_cast<u32>(t->format), t->baseAddress, t->resolveOwned,
                 t->resolveProvisional, t->uploaded, t->lastResolvedFrame, s.guest_frames,
                 t->host.width, t->host.height, static_cast<u32>(t->host.format));
      }
      if (Settings::DiagDump() && dumps < 16 && !t->host.isDepth && t->host.texture &&
          t->width >= 64 && std::find(dumped.begin(), dumped.end(), t) == dumped.end()) {
        dumped.push_back(t);
        ++dumps;
        ::mkdir("/app0/userdata/wos_recomp/dump", 0777);
        const std::string path =
            std::format("/app0/userdata/wos_recomp/dump/f{}_read{:04}_slot{}_tex{:x}_base{:x}.ppm",
                        s.guest_frames + 1, draw_no, slot, t->va, t->baseAddress);
        DumpHostTextureLocked(s, t->host, path.c_str(), 1.0f);
      }
    }
    // WoS port (fix37): the asset textures the frame samples, as the renderer decoded them
    // (first 64 distinct, colour, up to 1024 a side) - against the guest bytes the capture
    // now records (blob 1 of every texture a draw used), to tell decode bugs from blend/UV
    // ones (the title banner and pause panels: tiny tiled DXT textures drawn as noise)
    static u32 asset_dumps = 0;
    static std::vector<const GuestTexture *> assets_dumped;
    static u64 assets_for = 0;
    if (assets_for != s.guest_frames + 1) {
      assets_for = s.guest_frames + 1;
      asset_dumps = 0;
      assets_dumped.clear();
    }
    for (u32 slot = 0; slot < 16 && Settings::DiagDump(); ++slot) {
      GuestTexture *t = s.draw_bound_textures[slot];
      if (!t || t->resolveOwned || t->lastResolve || t->host.isDepth || !t->host.texture ||
          !t->uploaded || t->width > 2048 || t->height > 2048 || asset_dumps >= 64 ||
          std::find(assets_dumped.begin(), assets_dumped.end(), t) != assets_dumped.end())
        continue;
      assets_dumped.push_back(t);
      ++asset_dumps;
      ::mkdir("/app0/userdata/wos_recomp/dump", 0777);
      const std::string path = std::format(
          "/app0/userdata/wos_recomp/dump/f{}_asset{:02}_draw{:04}_slot{}_base{:x}_fmt{}_{}x{}{}.ppm",
          s.guest_frames + 1, asset_dumps, draw_no, slot, t->baseAddress, static_cast<u32>(t->format),
          t->width, t->height, t->tiled ? "_tiled" : "");
      DumpHostTextureLocked(s, t->host, path.c_str(), 1.0f);
      // fix45: the lower mips (and every cube face) raw, for the black mid-distance buildings
      static u32 mip_dumped_textures = 0;
      static u64 mip_dump_frame = 0;
      if (mip_dump_frame != s.guest_frames + 1) {
        mip_dump_frame = s.guest_frames + 1;
        mip_dumped_textures = 0;
      }
      const u32 fmt = static_cast<u32>(t->format);
      const bool bc = fmt == 18 || fmt == 19 || fmt == 20;
      if (t->host.mipLevels > 1 && (bc || fmt == 6) && mip_dumped_textures < 12) {
        ++mip_dumped_textures;
        const u32 bpb = fmt == 18 ? 8 : bc ? 16 : 4;
        const u32 layers = std::max(1u, t->host.arraySize);
        for (u32 layer = 0; layer < layers; ++layer)
          for (u32 level = layers > 1 ? 0u : 1u; level < std::min(t->host.mipLevels, 8u); ++level) {
            const std::string mp = std::format(
                "/app0/userdata/wos_recomp/dump/f{}_mip_base{:x}_fmt{}_{}x{}_l{}_face{}_of{}.raw",
                s.guest_frames + 1, t->baseAddress, fmt, std::max(1u, t->host.width >> level),
                std::max(1u, t->host.height >> level), level, layer, t->host.mipLevels);
            DumpHostTextureLevelRawLocked(s, t->host, level, layer, bpb, bc, mp.c_str());
          }
      }
    }
  }
}

void ExecuteDraw(u32 device_va, u32 prim, GeometryPlan &geom,
                 FloatConstantDirty constants, const u8 *device_image = nullptr) {
  auto &s = state();
  s.vs_float_constants_stale |= constants.vs;
  s.ps_float_constants_stale |= constants.ps;
  if (!s.ready)
    return;
  EOT_CPU_ZONE("ExecuteDraw");
  PerfScope perf_scope(s.perf.capture_ms);
  s.perf.draws++;
  // WoS port (fix77): test - the city's facade LOD draws (vertex shaders using winTexWindows*)
  // whose object lies farther than wos_facade_max_distance (view depth of the object's origin =
  // worldViewProj row 3 .w, VS c3.w) are not drawn - none of the per-draw capture work runs.
  // 0 = off. Every 600 frames: the facade draws' distance percentiles and how many were skipped.
  static const float max_distance = static_cast<float>(wos_facade_max_distance());
  static const bool facade_stats = max_distance > 0.0f || wos_diag_extra();
  if (facade_stats) {
    static const u64 kFacadeVs[] = {0x0696834fc6696f33ull, 0x11b4ede8fad8f9beull, 0x168c9332dec452b3ull, 0x209dbaee6495a6edull, 0x3a1c0367e8af41e8ull, 0x484c7df7518e7c99ull, 0x5510777656e888dcull, 0x5999c0c01e7bc438ull, 0x5e7a3cf672b61367ull, 0x65276691b5432ce9ull, 0x6fa318df9e0ff0caull, 0x83f807cadc2e152bull, 0x8f3c146bc94fa12eull, 0x925f3a2140d582c9ull, 0x98295ce9745a3cecull, 0x9bde15ad72228497ull, 0xa34717da2ac91573ull, 0xb0b283b09cbc2f53ull, 0xb5cb84735662fd54ull, 0xba677358934efae7ull, 0xc242b896e8d781deull, 0xc69747dcbcf7ee02ull, 0xcc5fbe50269c04adull, 0xce0e92be6300e728ull, 0xe6b6112918c92088ull, 0xfa297d4f117b5763ull};
    struct FacadeStats {
      std::vector<float> w;
      u64 frame = 0, skipped = 0, total = 0;
    };
    static FacadeStats stats;
    const u32 vs_va = mem::load<u32>(device_va + dev::kVertexShader);
    GuestShader *vs = vs_va ? FindGuestShader(s, vs_va) : nullptr;
    bool facade = false;
    if (vs)
      for (u64 h : kFacadeVs)
        facade |= vs->hash == h;
    if (facade) {
      const u32 bits = mem::load<u32>(device_va + dev::kVsFloatConstants + 3 * 16 + 12);
      f32 w;
      std::memcpy(&w, &bits, 4);
      ++stats.total;
      if (stats.w.size() < 65536)
        stats.w.push_back(w);
      if (s.guest_frames >= stats.frame + 600 && !stats.w.empty()) {
        std::sort(stats.w.begin(), stats.w.end());
        auto pct = [&](f64 p) { return stats.w[std::min<size_t>(stats.w.size() - 1, size_t(p * stats.w.size()))]; };
        EOT_INFO("[facade-lod] {} facade draws: distance p25 {:.0f} p50 {:.0f} p75 {:.0f} p90 {:.0f} "
                 "max {:.0f} | limit {:.0f}: {} skipped ({:.0f}%)",
                 stats.total, pct(0.25), pct(0.5), pct(0.75), pct(0.9), stats.w.back(), max_distance,
                 stats.skipped, stats.total ? 100.0 * stats.skipped / stats.total : 0.0);
        stats.w.clear();
        stats.frame = s.guest_frames;
        stats.skipped = stats.total = 0;
      }
      if (max_distance > 0.0f && w > max_distance) {
        ++stats.skipped;
        return;
      }
    }
  }
  if (RenderThreadActive()) {
    {
      RenderEnqueue enqueue;
      RenderCommand &c = enqueue.cmd();
      c.type = RenderCommandType::Draw;
      g_capture_queued = true;  // fix117
      const bool captured = CaptureDraw(s, device_va, prim, geom, device_image, c.draw);
      g_capture_queued = false;
      if (captured)
        enqueue.commit(false);
    }
    PoolDrainRetired(s, vertex_mirrors().pool);
    return;
  }
#if defined(__PROSPERO__)
  // The PS5 aligns thread-locals to 32 bytes at most, and the packet's device
  // window wants 64: each thread's packet lives on the heap instead
  static thread_local std::unique_ptr<DrawPacket> packet_storage;
  if (!packet_storage)
    packet_storage = std::make_unique<DrawPacket>();
  DrawPacket &packet = *packet_storage;
#else
  static thread_local DrawPacket packet;
#endif
  const bool captured = CaptureDraw(s, device_va, prim, geom, device_image, packet);
  PoolDrainRetired(s, vertex_mirrors().pool);
  if (!captured)
    return;
  std::lock_guard video_lock(s.mutex);
  ReplayDrawLocked(s, packet);
}

}


// WoS port (fix111): the render thread's half of a draw's capture, run in queue order before
// any draw of the batch replays (draws are sorted after this): the constant files' byte swap
// (wos_defer_constants) and the vertex views (wos_defer_vertices). false = drop the draw.
bool FinishDeferredDraw(VideoState &s, DrawPacket &pk) {
  for (u32 stage = 0; stage < 2; ++stage) {
    if (!pk.deferConstSwap[stage])
      continue;
    pk.deferConstSwap[stage] = false;
    UploadAlloc &a = stage ? pk.ps_consts : pk.vs_consts;
    if (!a.cpu)
      continue;
    u32 *marker = reinterpret_cast<u32 *>(a.cpu - kRawConstHeader);
    if (*marker)
      continue;  // already swapped for an earlier draw that shares this upload
    *marker = 1;
    const u32 dwords = static_cast<u32>(a.size / 4);
    auto *w = reinterpret_cast<u32 *>(a.cpu);
    for (u32 i = 0; i < dwords; ++i)
      w[i] = __builtin_bswap32(w[i]);
    if (wos_stack_garbage_zero())
      for (u32 i = 0; i < dwords; ++i)
        if (w[i] == 0xBEBEBEBEu)
          w[i] = 0;
  }
  if (!pk.deferVertices)
    return true;
  pk.deferVertices = false;
  StreamInfo streams[16];
  for (u32 S = 0; S <= pk.max_slot && S < 16; ++S) {
    const DrawPacket::DeferredStream &d = pk.dStreams[S];
    streams[S].stream = d.stream;
    streams[S].stride = d.stride;
    streams[S].data = d.data;
    streams[S].sizeBytes = d.sizeBytes;
    streams[S].dataVa = d.dataVa;
    streams[S].objectVa = d.objectVa;
    if (d.verifyHash && d.data && d.stride) {
      const u64 first = u64(pk.dLo) * d.stride;
      u64 bytes = (u64(pk.dHi) - pk.dLo + 1) * d.stride;
      if (d.sizeBytes && first + bytes > d.sizeBytes)
        bytes = first < d.sizeBytes ? d.sizeBytes - first : 0;
      const u64 now = bytes ? (XXH3_64bits(d.data + first, static_cast<size_t>(bytes)) | 1) : 1;
      static std::atomic<u64> checked{0}, changed{0};
      const u64 c = checked.fetch_add(1, std::memory_order_relaxed) + 1;
      if (now != d.verifyHash) {
        const u64 n = changed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 40 || n % 1000 == 0)
          EOT_WARN("[defer-verify] vs {:016x} stream {} va {:#x}+{} (stride {}): the vertices changed "
                   "between the draw and the render thread ({} of {} checked ranges)",
                   pk.vs ? pk.vs->hash : 0, S, d.dataVa, bytes, d.stride, n, c);
      }
      if ((c & 0xFFFF) == 0)
        EOT_INFO("[defer-verify] {} ranges checked, {} changed before the render thread read them",
                 c, changed.load(std::memory_order_relaxed));
    }
  }
  PerfScope copy_scope(s.perf.stream_ms);
  return BuildVertexViews(s, pk, pk.layout, streams, pk.dLo, pk.dHi, false, nullptr, pk.dInstMask,
                          pk.dInstN, pk.vs ? pk.vs->hash : 0);
}

void ReplayDrawLocked(VideoState &s, const DrawPacket &pk) {
  if (!s.ready)
    return;
  PerfScope perf_scope(s.perf.draw_ms);
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  ReplayDraw(s, pk);
}

void FlushGeometryStaging(VideoState &s) {
  if (!s.command_list_open)
    return;
  PoolFlushToVram(s, index_pool(), plume::RenderBufferFlag::INDEX);
  PoolFlushToVram(s, vertex_mirrors().pool, plume::RenderBufferFlag::VERTEX);
}

void GeometryCacheBytes(GeometryCacheSizes *out) {
  *out = GeometryCacheSizes{};
  auto sum = [](BufferPool &pool, u64 &upload, u64 &vram, u32 &chunks) {
    std::lock_guard lock(pool.mutex);
    for (const auto &ch : pool.chunks) {
      upload += ch.buffer ? ch.capacity : 0;
      vram += ch.vram ? ch.capacity : 0;
      chunks++;
    }
  };
  sum(index_pool(), out->indexUpload, out->indexVram, out->indexChunks);
  sum(vertex_mirrors().pool, out->vertexUpload, out->vertexVram, out->vertexChunks);
}

void FlushPendingTransitions(VideoState &s) {
  if (!s.pending_transition_count)
    return;
  const u32 n = s.pending_transition_count;
  s.pending_transition_count = 0;
  TransitionManyLocked(s, s.pending_transitions, n);
}
namespace {

}

// WoS port (fix116): wos_facade_occlusion - the city's window strips that are hidden behind
// nearer ones are not drawn.
//
// A capture of one city frame (4753 draws): 2812 of them are DrawVertices quad lists of the
// facade pair VS 168c9332dec452b3 / PS c447a6667bf82a6d (other places use 7 more vertex
// shaders with the same position, e.g. a34717da2ac91573) - every one a single column of 1-13
// window cells of one wall (4-byte vertices: cell x, y in bytes 3 and 2; position =
// worldViewProj * (x, y, 0, 1)), opaque (the shader writes alpha 1, no discard), depth tested
// and written. The game culls the walls that face away and those off screen itself, but not
// the ones behind other buildings: about 30% of the strips in that frame.
//
// Each strip is one screen quad. A low-resolution depth buffer (384x216, the farthest w of
// the nearest strip per pixel, strips sampled at pixel centres) takes every strip drawn; a
// strip whose bounding box, grown by a pixel, lies wholly behind what is there (2% margin)
// is hidden and is skipped. Strips arrive roughly front to back, so the ones before a strip
// are nearly all that can hide it. The buffer is one view's: it starts again each frame and
// whenever the view (worldViewProj * world^-1) changes - a reflection or shadow pass.
// 1 = the renderer skips the draw (the game's own D3D call still runs); 2 = the game's call
// is skipped too (wos_ps5_native.cpp).
namespace {

struct FacadeOcclusion {
  static constexpr int kW = 384, kH = 216;
  float zb[kW * kH];
  float vp[4][4] = {};
  u64 frame = ~0ull;
  bool haveView = false;
  u32 viewsThisFrame = 0;
  // the last strip's c0-c6 as the game has them, and worldViewProj decoded: a wall's
  // strips share them (so the view was checked already)
  u8 lastConstants[112] = {};
  float lastM[4][4] = {};
  bool lastValid = false;
  u64 tested = 0, hidden = 0, offscreen = 0, drawn = 0, views = 0, statFrame = 0;
  struct ShaderCache {
    u64 generation = 0;
    u32 vs = 0, ps = 0;
    bool facade = false;
  } shaders;
};

inline float FacadeFloat(const u8 *p) {
  u32 v;
  std::memcpy(&v, p, 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

}

bool WosFacadeTest(u32 device_va, u32 start, u32 count, f64 *cost_ms);
// why a quad-list draw was not tested (for the log): the n-th "no" in WosFacadeTest
static u32 g_facade_no[24];
static u64 g_facade_seen[4];  // the last draw's vs and ps hashes, depth control, fetch
static std::atomic<bool> g_facade_restart{false};  // a new owner thread: forget the view
inline bool FacadeNo(u32 why) {
  ++g_facade_no[why < 24 ? why : 23];
  return false;
}

bool WosFacadeHidden(u32 device_va, u32 prim, u32 start, u32 count) {
  if (prim != kPrimQuadList || count < 4 || count > 1024 || (count & 3))
    return false;
  // One thread at a time (the game draws from one): plain statics, not thread-locals,
  // which cost a call each on the PS5. Another thread takes over only when the owner
  // has not drawn for two frames.
  static std::atomic<uintptr_t> owner{0};
  static u64 owner_frame = 0;
  static u32 calls = 0;
  static f64 cost_ms = 0.0;
  {
    const uintptr_t me = reinterpret_cast<uintptr_t>(pthread_self());
    uintptr_t current = owner.load(std::memory_order_relaxed);
    const u64 frame = state().captured_presents;
    if (current != me) {
      if (current != 0 && owner_frame + 2 > frame)
        return false;
      if (!owner.compare_exchange_strong(current, me))
        return false;
      g_facade_restart.store(true, std::memory_order_relaxed);
    }
    owner_frame = frame;
  }
  if (calls == 0)
    EOT_INFO("[facade-occlusion] on, level {}", wos_facade_occlusion());
  if ((calls & 0xFFFFFu) == 0x20000u) {
    std::string line;
    for (u32 i = 1; i < 24; ++i)
      if (g_facade_no[i])
        line += std::format(" #{}:{}", i, g_facade_no[i]);
    EOT_INFO("[facade-occlusion] {} quad-list draws; not tested because{} | last vs {:016x} ps {:016x} "
             "depth {:#x} fetch {:#x}",
             calls, line.empty() ? " (none)" : line, g_facade_seen[0], g_facade_seen[1],
             g_facade_seen[2], g_facade_seen[3]);
  }
  if ((++calls & 15u) != 0)
    return WosFacadeTest(device_va, start, count, &cost_ms);
  const u64 t0 = PerfNow();
  const bool hidden = WosFacadeTest(device_va, start, count, &cost_ms);
  cost_ms += 16.0 * static_cast<f64>(PerfNow() - t0) * PerfMsPerTick();
  return hidden;
}

bool WosFacadeTest(u32 device_va, u32 start, u32 count, f64 *cost_ms) {
  auto &s = state();
  if (!s.ready)
    return FacadeNo(1);
  // (the owner thread's, see WosFacadeHidden)
  static FacadeOcclusion *storage = new FacadeOcclusion();
  FacadeOcclusion &o = *storage;
  if (g_facade_restart.exchange(false, std::memory_order_relaxed)) {
    o.haveView = false;
    o.lastValid = false;
    o.shaders = {};
  }

  // the facade's shader pair
  const u32 vs_va = mem::load<u32>(device_va + dev::kVertexShader);
  const u32 ps_va = mem::load<u32>(device_va + dev::kPixelShader);
  const u64 generation = s.shader_generation.load(std::memory_order_acquire);
  if (o.shaders.generation != generation || o.shaders.vs != vs_va || o.shaders.ps != ps_va) {
    GuestShader *vs = vs_va ? FindGuestShader(s, vs_va) : nullptr;
    GuestShader *ps = ps_va ? FindGuestShader(s, ps_va) : nullptr;
    o.shaders.vs = vs_va;
    o.shaders.ps = ps_va;
    // The vertex shaders with this position (one 4-byte input, worldViewProj * (x, y, 0, 1),
    // world at c4): 8 of the 26 facade shaders. The pixel shaders seen with them, each
    // writing alpha 1 with no discard. Any other pair is drawn as ever.
    static constexpr u64 kStripVs[] = {0x0696834fc6696f33ull, 0x168c9332dec452b3ull, 0x484c7df7518e7c99ull,
                                       0x98295ce9745a3cecull, 0x9bde15ad72228497ull, 0xa34717da2ac91573ull,
                                       0xb5cb84735662fd54ull, 0xba677358934efae7ull};
    static constexpr u64 kStripPs[] = {0xc447a6667bf82a6dull, 0x6075f07454832b24ull, 0x5acb3796e01eed3cull};
    bool vs_ok = false, ps_ok = false;
    if (vs && ps) {
      for (u64 h : kStripVs)
        vs_ok |= vs->hash == h;
      for (u64 h : kStripPs)
        ps_ok |= ps->hash == h;
    }
    o.shaders.facade = vs_ok && ps_ok;
    g_facade_seen[0] = vs ? vs->hash : 0;
    g_facade_seen[1] = ps ? ps->hash : 0;
    g_facade_seen[2] = mem::load<u32>(device_va + dev::kDepthControl);
    g_facade_seen[3] = mem::load<u32>(device_va + dev::StreamFetchSlotOffset(0));
    // a shader not seen yet is looked up again next time
    o.shaders.generation = vs && ps ? generation : 0;
  }
  if (!o.shaders.facade)
    return FacadeNo(2);
  // depth tested and written; 4-byte vertices in stream 0
  if ((mem::load<u32>(device_va + dev::kDepthControl) & 0x6) != 0x6)
    return FacadeNo(3);
  const u32 slot = dev::StreamFetchSlotOffset(0);
  const u32 d0 = mem::load<u32>(device_va + slot), d1 = mem::load<u32>(device_va + slot + 4);
  const u8 *stride = mem::at<u8>(device_va + dev::kStreamStride0);
  if ((d0 & 3) != 3 || !stride || *stride != 1)
    return FacadeNo(4);
  if ((u64(start) + count) * 4 > u64((d1 >> 2) & 0xFFFFFF) * 4)
    return FacadeNo(5);
  const u8 *v = mem::phys<u8>(d0 & ~3u);
  const u8 *cb = mem::at<u8>(device_va + dev::kVsFloatConstants);
  if (!v || !cb)
    return FacadeNo(6);
  v += 4ull * start;

  // one filled rectangle of cells
  u32 x0 = 255, x1 = 0, y0 = 255, y1 = 0;
  for (u32 i = 0; i < count; ++i) {
    const u32 x = v[4 * i + 3], y = v[4 * i + 2];
    x0 = std::min(x0, x);
    x1 = std::max(x1, x);
    y0 = std::min(y0, y);
    y1 = std::max(y1, y);
  }
  if ((x1 - x0) * (y1 - y0) != count / 4)
    return FacadeNo(7);

  float m[4][4];
  if (o.lastValid && o.haveView && o.frame == s.captured_presents &&
      std::memcmp(o.lastConstants, cb, sizeof o.lastConstants) == 0) {
    std::memcpy(m, o.lastM, sizeof m);  // the same wall as the strip before
  } else {
    o.lastValid = false;
    // worldViewProj c0-c3, world c4-c6 (affine); the view = worldViewProj * world^-1 (the
    // shader's "worldInverse", c7-c9, is not that: its translation is another)
    float w[3][4], wi[3][4], vp[4][4];
    for (u32 r = 0; r < 4; ++r)
      for (u32 c = 0; c < 4; ++c)
        m[r][c] = FacadeFloat(cb + 16 * r + 4 * c);
    for (u32 r = 0; r < 3; ++r)
      for (u32 c = 0; c < 4; ++c)
        w[r][c] = FacadeFloat(cb + 16 * (4 + r) + 4 * c);
    {
      const float c00 = w[1][1] * w[2][2] - w[1][2] * w[2][1], c01 = w[1][2] * w[2][0] - w[1][0] * w[2][2],
                  c02 = w[1][0] * w[2][1] - w[1][1] * w[2][0];
      const float det = w[0][0] * c00 + w[0][1] * c01 + w[0][2] * c02;
      if (!(std::fabs(det) > 1.0e-12f) || !(std::fabs(det) < 1.0e12f))
        return FacadeNo(8);
      const float id = 1.0f / det;
      wi[0][0] = c00 * id;
      wi[0][1] = (w[0][2] * w[2][1] - w[0][1] * w[2][2]) * id;
      wi[0][2] = (w[0][1] * w[1][2] - w[0][2] * w[1][1]) * id;
      wi[1][0] = c01 * id;
      wi[1][1] = (w[0][0] * w[2][2] - w[0][2] * w[2][0]) * id;
      wi[1][2] = (w[0][2] * w[1][0] - w[0][0] * w[1][2]) * id;
      wi[2][0] = c02 * id;
      wi[2][1] = (w[0][1] * w[2][0] - w[0][0] * w[2][1]) * id;
      wi[2][2] = (w[0][0] * w[1][1] - w[0][1] * w[1][0]) * id;
      for (u32 r = 0; r < 3; ++r)
        wi[r][3] = -(wi[r][0] * w[0][3] + wi[r][1] * w[1][3] + wi[r][2] * w[2][3]);
    }
    for (u32 r = 0; r < 4; ++r)
      for (u32 c = 0; c < 4; ++c)
        vp[r][c] = m[r][0] * wi[0][c] + m[r][1] * wi[1][c] + m[r][2] * wi[2][c] + (c == 3 ? m[r][3] : 0.0f);
    bool same = o.haveView && o.frame == s.captured_presents;
    if (same)
      for (u32 r = 0; r < 4 && same; ++r)
        for (u32 c = 0; c < 4; ++c) {
          const float a = vp[r][c], b = o.vp[r][c];
          const float tol = (c == 3 ? 2.0f : 0.02f) + 0.01f * std::fabs(b);
          if (!(std::fabs(a - b) <= tol)) {
            same = false;
            break;
          }
        }
    if (!same) {
      if (o.frame != s.captured_presents) {
        o.frame = s.captured_presents;
        o.viewsThisFrame = 0;
        if (o.frame >= o.statFrame + 600) {
          if (o.tested)
            EOT_INFO("[facade-occlusion] {} strips tested: {} hidden ({:.0f}%), {} off screen, {} drawn | "
                     "{} views in {} frames | the test: {:.2f} ms a frame",
                     o.tested, o.hidden, 100.0 * o.hidden / o.tested, o.offscreen, o.drawn, o.views,
                     o.frame - o.statFrame, *cost_ms / static_cast<f64>(o.frame - o.statFrame));
          o.tested = o.hidden = o.offscreen = o.drawn = o.views = 0;
          *cost_ms = 0.0;
          o.statFrame = o.frame;
        }
      }
      // views that alternate draw by draw: not worth a buffer each
      if (++o.viewsThisFrame > 12) {
        o.haveView = false;
        return FacadeNo(9);
      }
      std::fill(std::begin(o.zb), std::end(o.zb), 3.0e38f);
      std::memcpy(o.vp, vp, sizeof vp);
      o.haveView = true;
      ++o.views;
    }
    std::memcpy(o.lastConstants, cb, sizeof o.lastConstants);
    std::memcpy(o.lastM, m, sizeof m);
    o.lastValid = true;
  }

  // the strip's four corners on screen
  const float cx[4] = {float(x0), float(x1), float(x1), float(x0)};
  const float cy[4] = {float(y0), float(y0), float(y1), float(y1)};
  float sx[4], sy[4], wmin = 3.0e38f, wmax = 0.0f;
  for (u32 i = 0; i < 4; ++i) {
    const float X = m[0][0] * cx[i] + m[0][1] * cy[i] + m[0][3];
    const float Y = m[1][0] * cx[i] + m[1][1] * cy[i] + m[1][3];
    const float W = m[3][0] * cx[i] + m[3][1] * cy[i] + m[3][3];
    if (!(W > 0.05f) || !(W < 1.0e7f))
      return FacadeNo(10);  // at or behind the camera: drawn, and no cover for others
    const float inv = 1.0f / W;
    sx[i] = (X * inv * 0.5f + 0.5f) * FacadeOcclusion::kW;
    sy[i] = (0.5f - Y * inv * 0.5f) * FacadeOcclusion::kH;
    if (!(std::fabs(sx[i]) < 1.0e6f) || !(std::fabs(sy[i]) < 1.0e6f))
      return FacadeNo(11);
    wmin = std::min(wmin, W);
    wmax = std::max(wmax, W);
  }
  const float fx0 = std::min(std::min(sx[0], sx[1]), std::min(sx[2], sx[3]));
  const float fx1 = std::max(std::max(sx[0], sx[1]), std::max(sx[2], sx[3]));
  const float fy0 = std::min(std::min(sy[0], sy[1]), std::min(sy[2], sy[3]));
  const float fy1 = std::max(std::max(sy[0], sy[1]), std::max(sy[2], sy[3]));
  ++o.tested;
  constexpr int kW = FacadeOcclusion::kW, kH = FacadeOcclusion::kH;
  // wholly outside the view (5% to spare)
  if (fx1 < -0.05f * kW || fx0 > 1.05f * kW || fy1 < -0.05f * kH || fy0 > 1.05f * kH) {
    ++o.offscreen;
    return true;
  }
  // hidden: every pixel of its box, grown by one, is covered by something nearer
  {
    const int bx0 = std::max(int(std::floor(fx0)) - 1, 0), bx1 = std::min(int(std::ceil(fx1)) + 1, kW);
    const int by0 = std::max(int(std::floor(fy0)) - 1, 0), by1 = std::min(int(std::ceil(fy1)) + 1, kH);
    if (bx1 > bx0 && by1 > by0 && (bx1 - bx0) * (by1 - by0) <= 8192) {
      const float limit = wmin / 1.02f - 0.25f;
      bool hid = true;
      for (int y = by0; y < by1 && hid; ++y) {
        const float *row = o.zb + y * kW;
        for (int x = bx0; x < bx1; ++x)
          if (!(row[x] < limit)) {
            hid = false;
            break;
          }
      }
      if (hid) {
        ++o.hidden;
        return true;
      }
    }
  }
  // drawn: it covers the pixels whose centres it holds, as far as its farthest corner
  ++o.drawn;
  {
    const int bx0 = std::max(int(std::floor(fx0)), 0), bx1 = std::min(int(std::ceil(fx1)), kW);
    const int by0 = std::max(int(std::floor(fy0)), 0), by1 = std::min(int(std::ceil(fy1)), kH);
    float ea[4], eb[4], ec[4];
    float area = 0.0f;
    for (u32 j = 0; j < 4; ++j) {
      const u32 k = (j + 1) & 3;
      ea[j] = -(sy[k] - sy[j]);
      eb[j] = sx[k] - sx[j];
      ec[j] = -(ea[j] * sx[j] + eb[j] * sy[j]);
      area += sx[j] * sy[k] - sx[k] * sy[j];
    }
    if (area < 0.0f)
      for (u32 j = 0; j < 4; ++j) {
        ea[j] = -ea[j];
        eb[j] = -eb[j];
        ec[j] = -ec[j];
      }
    float er[4];  // -1 / a, for the span ends
    for (u32 j = 0; j < 4; ++j)
      er[j] = std::fabs(ea[j]) > 1.0e-6f ? -1.0f / ea[j] : 0.0f;
    if (std::fabs(area) > 1.0e-3f)
      for (int y = by0; y < by1; ++y) {
        float *row = o.zb + y * kW;
        const float py = float(y) + 0.5f;
        // the row's span: between the edges, each a half plane a*x + (b*y + c) >= 0
        float lo = float(bx0) + 0.5f, hi = float(bx1) - 0.5f;
        bool any = true;
        for (u32 j = 0; j < 4; ++j) {
          const float k = eb[j] * py + ec[j];
          if (ea[j] > 1.0e-6f)
            lo = std::max(lo, k * er[j]);
          else if (ea[j] < -1.0e-6f)
            hi = std::min(hi, k * er[j]);
          else if (k < 0.0f)
            any = false;
        }
        if (!any || !(lo <= hi))
          continue;
        // pixel centres x + 0.5 in [lo, hi]
        const int xa = std::max(int(std::ceil(lo - 0.5f)), bx0), xb = std::min(int(std::floor(hi - 0.5f)), bx1 - 1);
        for (int x = xa; x <= xb; ++x)
          row[x] = std::min(row[x], wmax);
      }
  }
  return false;
}

void DrawGuestPrimitives(u32 device_va, u32 prim, u32 start_vertex, u32 vertex_count,
                         FloatConstantDirty constants) {
  {
    static const int facade_occlusion = wos_facade_occlusion();
    if (facade_occlusion == 1 && prim == kPrimQuadList &&
        WosFacadeHidden(device_va, prim, start_vertex, vertex_count)) {
      auto &s = state();
      s.vs_float_constants_stale |= constants.vs;
      s.ps_float_constants_stale |= constants.ps;
      return;
    }
  }
  if (!vertex_count) {
    auto &s = state();
    std::lock_guard lock(s.guest_mutex);
    s.vs_float_constants_stale |= constants.vs;
    s.ps_float_constants_stale |= constants.ps;
    return;
  }
  GeometryPlan g;
  bool expand = false;
  g.topology = ConvertPrimitiveType(prim, &expand);
  g.startVertex = start_vertex;
  g.vertexCount = vertex_count;
  if (prim == kPrimRectList) {
    g.rectList = true;
  } else if (expand) {
    static const int strip_fast = wos_strip_fast_path();
    if (strip_fast >= 1 && prim == kPrimQuadList && vertex_count <= kQuadPatternVertices &&
        !(vertex_count & 3) && RenderThreadActive()) {
      // fix117: the shared quad pattern - (0 1 2, 0 2 3) + 4q from start_vertex
      g.quadPattern = true;
      g.baseVertex = static_cast<i32>(start_vertex);
    } else {
      g.indices.resize(vertex_count);
      for (u32 i = 0; i < vertex_count; ++i)
        g.indices[i] = start_vertex + i;
      ExpandIndices(prim, g.indices, false);
      g.baseVertex = 0;
    }
    g.indexed = true;
    g.topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  ExecuteDraw(device_va, prim, g, constants);
}

// fix118: the ranges as one indexed draw - each range's primitives, the gaps between them
// skipped by the index list (the vertices in the gaps are copied with the rest, unused)
void DrawGuestPrimitiveRanges(u32 device_va, u32 prim, const u32 *ranges, u32 range_count,
                              FloatConstantDirty constants) {
  GeometryPlan g;
  bool expand = false;
  g.topology = ConvertPrimitiveType(prim, &expand);
  const u32 per = prim == kPrimQuadList ? 4u : prim == 4 ? 3u : prim == 2 ? 2u : 1u;  // quad, triangle, line lists
  u32 total = 0, lo = 0xFFFFFFFFu;
  for (u32 r = 0; r < range_count; ++r) {
    total += ranges[2 * r + 1] / per * per;
    lo = std::min(lo, ranges[2 * r]);
  }
  if (!total || prim == kPrimRectList || (expand && prim != kPrimQuadList)) {
    // nothing to draw, or a primitive this path does not cover: one draw per range
    for (u32 r = 0; r < range_count; ++r)
      DrawGuestPrimitives(device_va, prim, ranges[2 * r], ranges[2 * r + 1], constants);
    return;
  }
  g.startVertex = lo;
  g.vertexCount = total;
  if (prim == kPrimQuadList) {
    g.indices.reserve(total / 4 * 6);
    for (u32 r = 0; r < range_count; ++r) {
      const u32 first = ranges[2 * r], quads = ranges[2 * r + 1] / 4;
      for (u32 q = 0; q < quads; ++q) {
        const u32 v = first + 4 * q;
        g.indices.insert(g.indices.end(), {v, v + 1, v + 2, v, v + 2, v + 3});
      }
    }
    g.topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  } else {
    g.indices.reserve(total);
    for (u32 r = 0; r < range_count; ++r) {
      const u32 first = ranges[2 * r], n = ranges[2 * r + 1] / per * per;
      for (u32 i = 0; i < n; ++i)
        g.indices.push_back(first + i);
    }
  }
  g.indexed = true;
  g.baseVertex = 0;
  ExecuteDraw(device_va, prim, g, constants);
}

void QueueGuestUpDraw(u32 device_va, u32 prim, u32 vertex_count, u32 stride, u32 data_va,
                      FloatConstantDirty constants) {
  FlushPendingUpDraw();
  auto &s = state();
  std::lock_guard lock(s.guest_mutex);
  s.pending_up.valid = data_va != 0 && vertex_count != 0;
  s.pending_up_armed.store(s.pending_up.valid, std::memory_order_release);
  s.pending_up.device_va = device_va;
  s.pending_up.primitive_type = prim;
  s.pending_up.vertex_count = vertex_count;
  s.pending_up.stride = stride;
  s.pending_up.data_va = data_va;
  s.pending_up.vs_constants_dirty = constants.vs;
  s.pending_up.ps_constants_dirty = constants.ps;
  if (!s.pending_up.valid) {
    s.vs_float_constants_stale |= constants.vs;
    s.ps_float_constants_stale |= constants.ps;
  }
  s.pending_up.has_image = false;
  if (const u8 *image = mem::at<u8>(device_va)) {
    s.pending_up.device_image.assign(image, image + kDeviceSnapshotBytes);
    s.pending_up.has_image = true;
    // WoS port (fix36): the stream up to the vertex space BeginVertices reserved (data_va)
    wos_regs_overlay(device_va, s.pending_up.device_image.data(), kDeviceSnapshotBytes,
                     data_va > 1 ? data_va : 0);
  }
}

void FlushPendingUpDraw() {
  auto &s = state();
  if (!s.pending_up_armed.load(std::memory_order_acquire))
    return;
  VideoState::PendingUpDraw up;
  {
    std::lock_guard lock(s.guest_mutex);
    if (!s.pending_up.valid)
      return;
    up = s.pending_up;
    s.pending_up.valid = false;
    s.pending_up_armed.store(false, std::memory_order_relaxed);
  }
  GeometryPlan g;
  bool expand = false;
  g.topology = ConvertPrimitiveType(up.primitive_type, &expand);
  g.startVertex = 0;
  g.vertexCount = up.vertex_count;
  g.stream0OverrideVa = up.data_va;
  g.stream0OverrideStride = up.stride;
  if (up.primitive_type == kPrimRectList) {
    g.rectList = true;
  } else if (expand) {
    g.indices.resize(up.vertex_count);
    for (u32 i = 0; i < up.vertex_count; ++i)
      g.indices[i] = i;
    ExpandIndices(up.primitive_type, g.indices, false);
    g.indexed = true;
    g.topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  trace::Bump(trace::Counter::DrawVertices);
  ExecuteDraw(up.device_va, up.primitive_type, g,
              {up.vs_constants_dirty, up.ps_constants_dirty},
              up.has_image ? up.device_image.data() : nullptr);
}

void DrawGuestIndexedPrimitives(u32 device_va, u32 prim, i32 base_vertex, u32 start_index,
                                u32 index_count, FloatConstantDirty constants) {
  if (!index_count) {
    auto &s = state();
    std::lock_guard lock(s.guest_mutex);
    s.vs_float_constants_stale |= constants.vs;
    s.ps_float_constants_stale |= constants.ps;
    return;
  }
  auto &s = state();
  DeviceView dev = Device(device_va);
  GeometryPlan g;
  bool expand = false;
  g.topology = ConvertPrimitiveType(prim, &expand);
  g.indexed = true;
  g.baseVertex = base_vertex;
  g.startIndex = start_index;
  g.guestIndexCount = index_count;
  {
    PerfScope index_scope(s.perf.index_ms);
    if (const CachedIndexRange *cached =
            GetCachedIndexRange(s, dev.U32(dev::kIndexBuffer), prim, start_index, index_count)) {
      g.cached = cached;
      g.topology = cached->topology;
    }
  }
  if (g.cached) {
    ExecuteDraw(device_va, prim, g, constants);
    return;
  }
  {
    PerfScope index_scope(s.perf.index_ms);
    if (!ReadGuestIndices(dev.U32(dev::kIndexBuffer), start_index, index_count, g.indices)) {
      std::lock_guard lock(s.guest_mutex);
      s.vs_float_constants_stale |= constants.vs;
      s.ps_float_constants_stale |= constants.ps;
      Dropped("index buffer unreadable", 0x6020);
      return;
    }
    if (prim == kPrimRectList) {
      g.rectList = true;
    } else if (expand || prim == kPrimTriangleStrip || prim == kPrimLineStrip) {
      ExpandIndices(prim, g.indices, true);
      g.topology = prim == kPrimLineStrip ? plume::RenderPrimitiveTopology::LINE_LIST
                                          : plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    }
  }
  ExecuteDraw(device_va, prim, g, constants);
}

namespace {

bool CaptureClear(u32 device_va, u32 flags, u32 rect_va, u32 color_va, float z, u32 stencil,
                  ClearPacket &pk) {
  pk = ClearPacket{};
  pk.device_va = device_va;
  pk.flags = flags;
  pk.z = z;
  pk.stencil = stencil;
  DeviceView dev = Device(device_va);
  if (const u8 *live = mem::at<u8>(device_va)) {
    dev.snapshot = live;
    dev.snapshotSize = dev::kDeviceSize;
  }
  if (!CaptureTargetWords(dev, pk.targets, true))
    return false;
  if (rect_va) {
    pk.hasRect = true;
    for (u32 i = 0; i < 4; ++i)
      pk.rect[i] = mem::load<i32>(rect_va + 4 * i);
  }
  if (color_va) {
    for (u32 i = 0; i < 4; ++i)
      pk.rgba[i] = mem::f32at(color_va + 4 * i);
  }
  return true;
}

}

void ClearGuestTargets(u32 device_va, u32 flags, u32 rect_va, u32 color_va, float z, u32 stencil) {
  auto &s = state();
  if (!s.ready)
    return;
  if (RenderThreadActive()) {
    RenderEnqueue enqueue;
    RenderCommand &c = enqueue.cmd();
    c.type = RenderCommandType::Clear;
    if (CaptureClear(device_va, flags, rect_va, color_va, z, stencil, c.clear))
      enqueue.commit(false);
    return;
  }
  ClearPacket pk;
  if (!CaptureClear(device_va, flags, rect_va, color_va, z, stencil, pk))
    return;
  std::lock_guard video_lock(s.mutex);
  ReplayClearLocked(s, pk);
}



void ReplayClearLocked(VideoState &s, const ClearPacket &pk) {
  if (!s.ready)
    return;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  Targets targets;
  if (!ResolveTargetsFromWords(s, pk.targets, targets))
    return;
  const u32 flags = pk.flags, stencil = pk.stencil;
  const float z = pk.z;
  const float(&rgba)[4] = pk.rgba;
  const plume::RenderColor color(rgba[0], rgba[1], rgba[2], rgba[3]);
  plume::RenderRect rect;
  const plume::RenderRect *rects = nullptr;
  u32 rect_count = 0;
  bool whole = true;
  if (pk.hasRect) {
    const i32 gx0 = pk.rect[0], gy0 = pk.rect[1];
    const i32 gx1 = pk.rect[2], gy1 = pk.rect[3];
    whole = gx0 <= 0 && gy0 <= 0 && gx1 >= static_cast<i32>(targets.width) &&
            gy1 >= static_cast<i32>(targets.height);
    if (!whole) {
      const float k = targets.scale;
      rect = plume::RenderRect(ScalePxBy(gx0, k), ScalePxBy(gy0, k), ScalePxBy(gx1, k),
                               ScalePxBy(gy1, k));
      rects = &rect;
      rect_count = 1;
    }
  }
  const bool clear_depth = (flags & 0x10) != 0, clear_stencil = (flags & 0x20) != 0;
  ghost::NoteClear(s.guest_frames, targets, flags, whole);
  if (clear_depth && whole && targets.depth)
    velocity::OnDepthCleared(s, *targets.depth);
  auto *cmd = s.command_list;
  if (GpuTimingDiagActive(s)) {
    GpuTimingDiagMark(s, cmd, std::format("clear flags {:#x}", flags));
    EOT_INFO("[diag] clear flags {:#x} {} rt0={:#x} ds={:#x} {}x{} rgba=({:.2f},{:.2f},{:.2f},{:.2f}) z={:.3f} s={}",
             flags, whole ? "whole" : "rect", targets.colorCount ? targets.color[0]->va : 0,
             targets.depth ? targets.depth->va : 0, targets.width, targets.height, rgba[0], rgba[1],
             rgba[2], rgba[3], z, stencil & 0xFF);
  }
  auto clears_all = [&](const HostTexture &image) {
    return clear_depth && (clear_stencil || !FormatHasStencil(image.format));
  };
  auto clear_depth_image = [&](const HostTexture &image, const plume::RenderRect *r, u32 n) {
    const bool with_stencil = clear_stencil && FormatHasStencil(image.format);
    if (clear_depth || with_stencil)
      cmd->clearDepthStencil(clear_depth, with_stencil, z, stencil & 0xFF, r, n);
  };
  auto clear_image = [&](GuestSurface &surf, HostTexture &image) {
    if (whole && (!surf.isDepth || clears_all(image)))
      image.needsClear = false;
    HostTexture *colors[4] = {surf.isDepth ? nullptr : &image, nullptr, nullptr, nullptr};
    if (!BindImages(s, colors, surf.isDepth ? 0u : 1u, surf.isDepth ? &image : nullptr, true))
      return;
    if (surf.isDepth)
      clear_depth_image(image, rects, rect_count);
    else
      cmd->clearColor(0, color, rects, rect_count);
    surf.perfClears++;
  };
  for (u32 i = 0; i < targets.colorCount; ++i) {
    if (!(flags & (1u << i)))
      continue;
    GuestSurface &surf = *targets.color[i];
    HostTexture &owner = SurfaceContentImage(surf);
    const bool both = SurfaceHasSingle(surf) &&
                      (whole || surf.imagesAgree || surf.content != GuestSurface::Content::Drawn);
    if (both) {
      if (surf.host.valid())
        clear_image(surf, surf.host);
      clear_image(surf, surf.single);
    } else {
      clear_image(surf, owner);
    }
    NoteSurfaceClearedColor(surf, owner, rgba, whole, both);
  }
  if (targets.depth && (flags & 0x30)) {
    GuestSurface &surf = *targets.depth;
    if (whole && !targets.colorCount && clear_depth)
      SurfaceRedirectBegin(s, surf);
    if (surf.redirectMirror && surf.redirectMirror->host.valid()) {
      HostTexture &image = surf.redirectMirror->host;
      plume::RenderRect region(surf.redirectX, surf.redirectY,
                               surf.redirectX + static_cast<i32>(surf.host.width),
                               surf.redirectY + static_cast<i32>(surf.host.height));
      if (!whole)
        region = plume::RenderRect(rect.left + surf.redirectX, rect.top + surf.redirectY,
                                   rect.right + surf.redirectX, rect.bottom + surf.redirectY);
      if (BindImages(s, nullptr, 0, &image, true)) {
        if (GpuTimingDiagActive(s))
          GpuTimingDiagMark(s, cmd, std::format("redirect region clear {}x{}", surf.host.width,
                                                surf.host.height));
        clear_depth_image(image, &region, 1);
        surf.perfClears++;
      }
      NoteSurfaceClearedDepth(surf, z, static_cast<u8>(stencil & 0xFF), whole && clears_all(surf.host), false);
    } else {
      const bool both = SurfaceHasSingle(surf);
      if (surf.host.valid())
        clear_image(surf, surf.host);
      if (both)
        clear_image(surf, surf.single);
      NoteSurfaceClearedDepth(surf, z, static_cast<u8>(stencil & 0xFF), whole && clears_all(surf.host), both);
    }
  }
  s.bound_draw_targets_valid = false;
  DrainHostDebugMessages(s, "clear");
}

}

// WoS port (fix116): for the game's draw hook (wos_ps5_native.cpp), wos_facade_occlusion = 2
extern "C" bool wos_facade_hidden(uint32_t device, uint32_t prim, uint32_t start, uint32_t count) {
  return eot::gpu::WosFacadeHidden(device, prim, start, count);
}
