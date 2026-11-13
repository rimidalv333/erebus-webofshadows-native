// WebOfShadowsRecomp for the PS5 - the native renderer: Edge of Time's, drawing Web of Shadows.
//
// With wos_native_renderer = true the title runs with no emulated Xenos GPU at all, as
// Edge of Time does: the game's D3D calls are hooked (Edge of Time's src/gpu/hooks, in
// wos/eot/, renamed by wos/eot/wos_eot_names.h) and its renderer draws them with the
// game's shaders recompiled ahead of time by XenosRecomp (the shader cache built from
// the harvest: wos/eot/wos_eot.cmake). The game's own D3D still runs under every hook,
// so its bookkeeping stays exact; its command stream goes nowhere, and its waits on the
// GPU end at once (wos/app/wos_ps5_headless.cpp, which the renderer turns on as well).
//
// This file owns the overrides of Web of Shadows' D3D functions (sub_XXXXXXXX): each
// calls Edge of Time's hook when the renderer is on and the game's own function when it
// is off, so the emulated path is exactly as before unless the toml asks otherwise.
// Five of the hooked functions are overridden elsewhere and branch the same way: the
// draws (wos_ps5_native.cpp), Set{Pixel,Vertex}Shader (wos_ps5_shader_harvest.cpp) and
// Swap (wos_ps5_headless.cpp).
//
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>
#include <format>
#include <string>
#include <cstdint>
#include <memory>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/startup_prospero.h>
#include <rex/ui/window.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/wos_tiling.h"
#include "gpu/device.h"
#include "gpu/imgui_overlay.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/shaders/guest_shaders.h"

REXCVAR_DEFINE_BOOL(wos_span_tails, false, "WoS",
                    "Native renderer: parse the packets around data D3D reserved inside a command-stream span (experimental)");
REXCVAR_DEFINE_BOOL(wos_native_debug_log, false, "WoS",
                    "Native renderer: log its debug lines (resolves, texture mirrors, surfaces)");
REXCVAR_DEFINE_BOOL(wos_native_renderer, false, "WoS",
                    "Draw the game natively with Edge of Time's renderer instead of emulating "
                    "the Xbox 360 GPU");

extern "C" void wos_regs_overlay(uint32_t device, uint8_t *image, uint32_t image_bytes, uint32_t sync);

namespace wos::native_renderer {
void NoteDrawItem(uint32_t key, uint32_t device);  // fix92/94 (at the render item hook)
void NoteDrawConstantsAfter(uint32_t key, uint32_t device);  // fix98

namespace {
std::atomic<int> g_state{-1};  // -1 unknown, 0 off, 1 on
}

bool Enabled() {
  int s = g_state.load(std::memory_order_relaxed);
  if (s < 0) {
    s = REXCVAR_GET(wos_native_renderer) ? 1 : 0;
    g_state.store(s, std::memory_order_relaxed);
  }
  return s == 1;
}

std::unique_ptr<rex::ui::ImmediateDrawer> CreateOverlayDrawer() {
  return eot::gpu::CreateOverlayDrawer();
}

// Before the game's first instruction: the host device, the shader cache, the pipelines.
bool Start(rex::ui::Window *window) {
  // Which delivered change set this eboot carries: compare against the files sent
  REXLOG_INFO("[WoS-RENDERER] starting Edge of Time's renderer: {} recompiled shaders "
              "(revision fix115, built " __DATE__ " " __TIME__ ")",
              eot::gpu::GuestShaderCacheCount());
  // fix34: two code addresses, to line a fault.log up with this build's llvm-pie.elf
  // ("elf" = address - the code segment's start, the fault.log's "code=" line)
  REXLOG_INFO("[WoS-RENDERER] code anchors: wos_regs_overlay {:p}, Start {:p}",
              reinterpret_cast<const void *>(&wos_regs_overlay), reinterpret_cast<const void *>(&Start));
  if (REXCVAR_GET(wos_native_debug_log)) {
    rex::SetCategoryLevel(rex::log::eot(), spdlog::level::debug);
    REXLOG_INFO("[WoS-RENDERER] debug log on");
  }
  prospero_startup_log("wos: native renderer: creating the host device");
  if (!eot::gpu::Video::CreateHostDevice(window)) {
    REXLOG_ERROR("[WoS-RENDERER] the host device could not be created");
    prospero_startup_log("wos: native renderer: host device FAILED");
    return false;
  }
  if (!eot::gpu::GuestShadersInit()) {
    REXLOG_ERROR("[WoS-RENDERER] the shader cache could not be loaded");
  }
  eot::gpu::PsoCachePrecache();
  prospero_startup_log("wos: native renderer: ready");
  return true;
}

void Shutdown() {
  eot::gpu::Video::BeginShutdown();
  eot::gpu::Video::Shutdown();
}

}  // namespace wos::native_renderer

// --- The overrides -------------------------------------------------------------------

namespace wos::capture {
void Call(const char *name, const PPCContext &ctx, uint8_t *base, bool draw);
bool Active();
void NoteBlob(uint32_t kind, uint32_t key, const uint8_t *data, uint32_t bytes);
void NoteMeshDraw();
}
namespace wos::threads {
const PPCContext *CurrentContext();  // fix46 (wos_ps5_threads.cpp)
bool CurrentStackBounds(uint32_t *limit, uint32_t *base);
}

#define WOS_NATIVE_HOOK(sub, eot)                                       \
  extern "C" void __imp__##sub(PPCContext &ctx, uint8_t *base);         \
  extern "C" void eot(PPCContext &ctx, uint8_t *base);                  \
  extern "C" void sub(PPCContext &ctx, uint8_t *base) {                 \
    if (wos::native_renderer::Enabled()) {                              \
      wos::capture::Call(#eot, ctx, base, false);                       \
      eot(ctx, base);                                                   \
    } else {                                                            \
      __imp__##sub(ctx, base);                                          \
    }                                                                   \
  }

WOS_NATIVE_HOOK(sub_82ABE618, wos_eot_ClearF)
WOS_NATIVE_HOOK(sub_82AB8B98, wos_eot_SetViewport)
WOS_NATIVE_HOOK(sub_82AB8658, wos_eot_SetGammaRamp)
WOS_NATIVE_HOOK(sub_82AB8720, wos_eot_SetPWLGamma)
WOS_NATIVE_HOOK(sub_82AB48A8, wos_eot_CreatePixelShader)
WOS_NATIVE_HOOK(sub_82AB49B8, wos_eot_CreateVertexShader)
WOS_NATIVE_HOOK(sub_82AB4368, wos_eot_XGRegisterVertexShader)
WOS_NATIVE_HOOK(sub_82AB5048, wos_eot_UnlockResource)
WOS_NATIVE_HOOK(sub_82ABD328, wos_eot_SetTexture)
WOS_NATIVE_HOOK(sub_82AB8CC8, wos_eot_SetRenderTarget)
WOS_NATIVE_HOOK(sub_82AB9028, wos_eot_SetDepthStencilSurface)
WOS_NATIVE_HOOK(sub_82AB8540, wos_eot_SetIndices)
WOS_NATIVE_HOOK(sub_82AB8420, wos_eot_SetStreamSource)

// D3DDevice_BeginTiling (device, flags, count, tile rects {x1,y1,x2,y2}, clear colour,
// z in f1, ...): Web of Shadows' scene pass. The bound surfaces are drawn whole at the
// union of the tile rects (gpu/wos_tiling.h); the clear BeginTiling issues for one tile
// clears the whole surface. EndTiling (sub_82AC9170) resolves each tile through
// D3DDevice_Resolve, hooked above, and needs nothing more.
extern "C" void __imp__sub_82AC8CE0(PPCContext &ctx, uint8_t *base);
extern "C" void sub_82AC8CE0(PPCContext &ctx, uint8_t *base) {
  if (!wos::native_renderer::Enabled()) {
    __imp__sub_82AC8CE0(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32, count = ctx.r5.u32, rects = ctx.r6.u32;
  uint32_t width = 0, height = 0;
  for (uint32_t i = 0; i < count && i < 16 && rects; ++i) {
    width = std::max(width, eot::mem::load<uint32_t>(rects + 16 * i + 8));
    height = std::max(height, eot::mem::load<uint32_t>(rects + 16 * i + 12));
  }
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 2)
    REXLOG_INFO("[WoS-RENDERER] BeginTiling: {} tiles, extent {}x{}", count, width, height);
  wos::capture::Call("BeginTiling", ctx, base, false);
  eot::gpu::wos_tiling::Begin(device, width, height);
  eot::gpu::wos_tiling::SetClearWhole(true);
  __imp__sub_82AC8CE0(ctx, base);
  eot::gpu::wos_tiling::SetClearWhole(false);
}

// --- The GPU's register file, mirrored from the command stream (fix34) --------------
//
// What the Xbox 360 GPU draws with is its register file, written by the command stream -
// not D3D's shadow of it in the device. Web of Shadows' D3D flushes its own shadow at each
// draw (type-0 writes: constants from device + 0x780/0x1780, fetch constants from + 0x480
// (sub_82ACB9A0), render state from + 0x2880.. (sub_82ACB760)), but the world, character
// and particle passes set much of their state straight into the stream: every world draw
// of the fix33 run sampled the same five stale fetch constants (the previous frame's bloom
// target, the 1x1 luminance, a blank 32x8 DXT5 - no texture object in any slot), i.e. its
// real textures never reached the shadow this renderer reads.
//
// So this file runs a small command processor of its own over the stream (as rexglue's
// does: type-0/1 register writes, SET_CONSTANT of every kind, SET_CONSTANT2,
// SET_SHADER_CONSTANTS, LOAD_ALU_CONSTANT from memory, IM_LOAD shader loads, indirect
// buffers two levels deep) into a private register mirror per device, and each draw's
// captured device window (Edge of Time's per-draw copy, never the live device: writing
// the live fetch shadow crashed fix29-fix32) takes the mirror's texture fetch constants and
// material render state. ALU constants still go to the shadow as before (proven since fix13).
REXCVAR_DEFINE_BOOL(wos_stream_type0_constants, true, "WoS",
                    "Apply shader constants from the command stream's register writes");
REXCVAR_DEFINE_BOOL(wos_stream_type0_fetch, false, "WoS",
                    "(fix34: unused - fetch constants reach draws through the register mirror)");
REXCVAR_DEFINE_BOOL(wos_regs_textures, true, "WoS",
                    "Draws sample the textures the command stream bound (GPU register mirror)");
REXCVAR_DEFINE_BOOL(wos_regs_state, true, "WoS",
                    "Draws use the blend/depth/stencil/cull state the command stream set (GPU register "
                    "mirror; off: only counted and logged)");
REXCVAR_DEFINE_BOOL(wos_regs_vertex, false, "WoS",
                    "Draws read the vertex buffers the command stream bound (GPU register mirror)");
REXCVAR_DEFINE_BOOL(wos_regs_log, false, "WoS", "Log the GPU register mirror's findings (fix62: off by default)");
// fix60: the city's depth-only shells (no pixel shader, main depth) - drawn as on the console
// unless wos_recomp.toml sets wos_depth_shells = false (fix54-59 behaviour: skipped)
REXCVAR_DEFINE_BOOL(wos_depth_shells, true, "WoS", "Draw the depth-only building shells");
// fix67: resolves of D24FS8 depth keep the Xbox 360's 20e4 precision (facade self-shadowing)
// fix69: diagnostic - texture slots (bit n = slot n) left unbound in draws that use a cube map
// (the city's facade glass: slot 0 sky-light cube, 1 horizon, 3 reflection cube)
REXCVAR_DEFINE_INT32(wos_debug_null_slots, 0, "WoS", "Diagnostic: texture slot mask left unbound in cube-map draws");
extern "C" int wos_debug_null_slots() { return REXCVAR_GET(wos_debug_null_slots); }
// fix71: cube-map reflection shimmer diagnostics
REXCVAR_DEFINE_INT32(wos_debug_cube_lod_bias, 0, "WoS", "Diagnostic: extra mip levels of LOD bias on cube maps");
REXCVAR_DEFINE_BOOL(wos_cube_tail_clamp, false, "WoS", "fix70: keep cube maps above their packed mip tail");
extern "C" int wos_debug_cube_lod_bias() { return REXCVAR_GET(wos_debug_cube_lod_bias); }
extern "C" bool wos_cube_tail_clamp() { return REXCVAR_GET(wos_cube_tail_clamp); }
// fix74: the facade glass's city reflection (flashes on the PS5) - off by default
REXCVAR_DEFINE_BOOL(wos_facade_reflections, true, "WoS", "City reflections on the facade glass (flash on the PS5)");
extern "C" bool wos_facade_reflections() { return REXCVAR_GET(wos_facade_reflections); }
// fix78/fix80: the per-draw diagnostics added in fix66-77 ([lod-flip] mutex + map per draw,
// [glass], [facade-lod] stats) - their own switch since fix80, so a performance run with
// wos_diagnostics = true does not pay for them
REXCVAR_DEFINE_BOOL(wos_debug_draw_stats, false, "WoS",
                    "fix80: per-draw LOD-flip / glass / facade statistics (slow; flashing hunt only)");
extern "C" bool wos_diag_extra() { return REXCVAR_GET(wos_debug_draw_stats); }
// fix77: test - facade LOD draws farther than this (view depth, game units) are skipped; 0 = off
REXCVAR_DEFINE_INT32(wos_facade_max_distance, 0, "WoS", "Skip city facade draws farther than this (0 = off)");
extern "C" int wos_facade_max_distance() { return REXCVAR_GET(wos_facade_max_distance); }
REXCVAR_DEFINE_INT32(wos_facade_occlusion, 0, "WoS",
                     "fix116: city window strips hidden behind nearer ones are not drawn "
                     "(0 off, 1 the renderer skips them, 2 the game's own draw call is skipped too)");
extern "C" int wos_facade_occlusion() { return REXCVAR_GET(wos_facade_occlusion); }
REXCVAR_DEFINE_INT32(wos_strip_fast_path, 2, "WoS",
                     "fix117: cheaper quad-list draws (the city's window strips): 0 off, 1 one shared "
                     "index pattern, 2 also reuse the previous draw's derived state when the device "
                     "window is identical, 3 as 1 with that reuse checked and logged");
extern "C" int wos_strip_fast_path() { return REXCVAR_GET(wos_strip_fast_path); }
REXCVAR_DEFINE_INT32(wos_strip_merge, 1, "WoS",
                     "fix118: the game's window-strip lists: 0 drawn strip by strip as the game "
                     "does, 1 strips that follow each other in the vertex buffer drawn as one, "
                     "2 as 0 with the merge statistics logged");
extern "C" int wos_strip_merge() { return REXCVAR_GET(wos_strip_merge); }
// fix72: diagnostic - cube maps painted one colour per face
REXCVAR_DEFINE_BOOL(wos_debug_cube_faces, false, "WoS", "Diagnostic: paint each cube-map face a solid colour");
extern "C" bool wos_debug_cube_faces() { return REXCVAR_GET(wos_debug_cube_faces); }
// fix68: diagnostic - no shadow maps bound (everything lit)
REXCVAR_DEFINE_BOOL(wos_debug_no_shadow_maps, false, "WoS", "Diagnostic: leave the shadow maps unbound");
extern "C" bool wos_debug_no_shadow_maps() { return REXCVAR_GET(wos_debug_no_shadow_maps); }
REXCVAR_DEFINE_BOOL(wos_depth_float24, true, "WoS", "Resolve float depth at the Xbox 360's 20e4 precision");
extern "C" bool wos_depth_float24() { return REXCVAR_GET(wos_depth_float24); }
REXCVAR_DEFINE_BOOL(wos_fix_reused_resolve_memory, true, "WoS",
                    "fix82: textures streamed into memory once used by a resolve are uploaded, not black");
extern "C" bool wos_fix_reused_resolve_memory() { return REXCVAR_GET(wos_fix_reused_resolve_memory); }
REXCVAR_DEFINE_BOOL(wos_texture_slice_check, true, "WoS",
                    "fix83: re-check 1/32 of each used texture's bytes a frame (partial rewrites)");
extern "C" bool wos_texture_slice_check() { return REXCVAR_GET(wos_texture_slice_check); }
REXCVAR_DEFINE_BOOL(wos_debug_skip_shadow_casters, false, "WoS",
                    "fix84 diagnostic: draw nothing into the 1024x1024 shadow maps (everything lit)");
extern "C" bool wos_debug_skip_shadow_casters() { return REXCVAR_GET(wos_debug_skip_shadow_casters); }
REXCVAR_DEFINE_INT32(wos_dump_every, 150, "WoS", "fix85: eot_diag_dump mid-frame image every N colour draws");
REXCVAR_DEFINE_INT32(wos_dump_max, 10, "WoS", "fix85: at most this many mid-frame images");
extern "C" int wos_dump_every() { return REXCVAR_GET(wos_dump_every); }
extern "C" int wos_dump_max() { return REXCVAR_GET(wos_dump_max); }
REXCVAR_DEFINE_BOOL(wos_vertex_frame_memo, true, "WoS",
                    "fix99: a vertex range drawn again in the same frame reuses its first lookup");
extern "C" bool wos_vertex_frame_memo() { return REXCVAR_GET(wos_vertex_frame_memo); }
REXCVAR_DEFINE_BOOL(wos_stack_garbage_zero, true, "WoS",
                    "fix100: shader constants never written by the game (0xBEBEBEBE, the runtime's "
                    "stack fill) are uploaded as 0 - the dark terrain patch");
extern "C" bool wos_stack_garbage_zero() { return REXCVAR_GET(wos_stack_garbage_zero); }
REXCVAR_DEFINE_BOOL(wos_stale_alias_white, true, "WoS",
                    "fix101: a resolve target overwritten since by a resolve of another format "
                    "(scratch memory reused) is sampled as white - the dark street square");
extern "C" bool wos_stale_alias_white() { return REXCVAR_GET(wos_stale_alias_white); }
REXCVAR_DEFINE_BOOL(wos_ring_keeps_pending, false, "WoS",
                    "fix96: ring constants do not overwrite constants the game set since (pending)");
REXCVAR_DEFINE_BOOL(wos_fake_screen_extents, true, "WoS",
                    "fix91: answer the game's screen-extent queries (EVENT_WRITE_EXT) as the emulator does");
REXCVAR_DEFINE_INT32(wos_occlusion_fake_samples, 1000, "WoS",
                     "fix90: occlusion queries report this many visible samples (-1 = leave pending)");
REXCVAR_DEFINE_BOOL(wos_debug_no_cull, false, "WoS", "fix86 diagnostic: draw every triangle two-sided");
REXCVAR_DEFINE_BOOL(wos_debug_depth_always, false, "WoS", "fix86 diagnostic: colour draws ignore the depth test");
REXCVAR_DEFINE_BOOL(wos_debug_no_stencil, false, "WoS", "fix86 diagnostic: no stencil test");
extern "C" bool wos_debug_no_cull() { return REXCVAR_GET(wos_debug_no_cull); }
extern "C" bool wos_debug_depth_always() { return REXCVAR_GET(wos_debug_depth_always); }
extern "C" bool wos_debug_no_stencil() { return REXCVAR_GET(wos_debug_no_stencil); }
REXCVAR_DEFINE_BOOL(wos_debug_no_zshift, false, "WoS", "fix87 diagnostic: no EoT vertex-z shift for polygon offset");
REXCVAR_DEFINE_BOOL(wos_debug_depth_clamp, false, "WoS", "fix87 diagnostic: clamp depth instead of clipping");
extern "C" bool wos_debug_no_zshift() { return REXCVAR_GET(wos_debug_no_zshift); }
extern "C" bool wos_debug_depth_clamp() { return REXCVAR_GET(wos_debug_depth_clamp); }
REXCVAR_DEFINE_BOOL(wos_dump_small, true, "WoS", "fix89: a 1/8-size image after every colour draw of the L3+R3 dump frame");
extern "C" bool wos_dump_small() { return REXCVAR_GET(wos_dump_small); }
REXCVAR_DEFINE_INT32(wos_dump_small_from, 1, "WoS", "fix106: first draw of the dump frame with a 1/8 image");
REXCVAR_DEFINE_INT32(wos_dump_small_to, 4000, "WoS", "fix106: last draw of the dump frame with a 1/8 image (fix107: 4000 = whole frame)");
extern "C" int wos_dump_small_from() { return REXCVAR_GET(wos_dump_small_from); }
extern "C" int wos_dump_small_to() { return REXCVAR_GET(wos_dump_small_to); }
// WoS port (fix107): diagnosis switches for the first-act daytime corruption
REXCVAR_DEFINE_STRING(wos_skip_shaders, "", "WoS",
                      "fix107: comma-separated shader hash prefixes (vs or ps, e.g. 7f1ab07f,d76ede35) whose draws are not drawn");
extern "C" const char *wos_skip_shaders() {
  static std::string v = REXCVAR_GET(wos_skip_shaders);
  return v.c_str();
}
REXCVAR_DEFINE_BOOL(wos_window_wide, true, "WoS",
                    "fix107: false = the per-draw device window without fix104's 0x3080-0x31A0 block");
extern "C" bool wos_window_wide() { return REXCVAR_GET(wos_window_wide); }
REXCVAR_DEFINE_BOOL(wos_layout_signature, true, "WoS",
                    "fix107: false = vertex layout keys without fix103's input signature");
extern "C" bool wos_layout_signature() { return REXCVAR_GET(wos_layout_signature); }
REXCVAR_DEFINE_BOOL(wos_instancing, true, "WoS",
                    "fix108: shader-instanced meshes get their mesh / instance records per vertex (false = drawn as before, broken)");
extern "C" bool wos_instancing() { return REXCVAR_GET(wos_instancing); }
// WoS port (fix111): the capture split between the game thread and the render thread
REXCVAR_DEFINE_BOOL(wos_defer_vertices, true, "WoS",
                    "fix111: vertex stream reads, mirror lookups and copies on the render thread (false = on the game thread)");
extern "C" bool wos_defer_vertices() { return REXCVAR_GET(wos_defer_vertices); }
REXCVAR_DEFINE_BOOL(wos_defer_constants, true, "WoS",
                    "fix111: shader constant byte swap on the render thread (false = on the game thread)");
extern "C" bool wos_defer_constants() { return REXCVAR_GET(wos_defer_constants); }
REXCVAR_DEFINE_BOOL(wos_defer_verify, false, "WoS",
                    "fix111: hash each deferred vertex range at the draw and again on the render thread; log changes ([defer-verify])");
extern "C" bool wos_defer_verify() { return REXCVAR_GET(wos_defer_verify); }
REXCVAR_DEFINE_BOOL(wos_capture_prefetch, true, "WoS",
                    "fix111: prefetch the next render command slot for writing after each command");
extern "C" bool wos_capture_prefetch() { return REXCVAR_GET(wos_capture_prefetch); }
REXCVAR_DEFINE_BOOL(wos_fast_apply, true, "WoS",
                    "fix113: ring constants into the shadow a 16-dword block at a time (false = per dword, as fix112)");
bool wos_fast_apply_on() { return REXCVAR_GET(wos_fast_apply); }
REXCVAR_DEFINE_BOOL(wos_fast_capture, true, "WoS",
                    "fix80: performance changes to the per-draw capture (false = fix79 behaviour)");
extern "C" bool wos_fast_capture_on() { return REXCVAR_GET(wos_fast_capture); }
REXCVAR_DEFINE_BOOL(wos_lean_stream, false, "WoS",
                    "fix104: lighter stream mirror on the draw thread (plain diagnostic counters, "
                    "one-copy constant apply) - same results");
extern "C" bool wos_lean_stream_on() { return REXCVAR_GET(wos_lean_stream); }
REXCVAR_DEFINE_BOOL(wos_overlay_memo, true, "WoS", "fix61/62: reuse the register overlay's result between draws");
REXCVAR_DEFINE_BOOL(wos_overlay_memo_verify, false, "WoS", "fix64: check every overlay memo hit the slow way and log differences");
REXCVAR_DEFINE_BOOL(wos_overlay_delta, false, "WoS",
                    "fix104: the register overlay recomputes only the texture slots / vertex streams "
                    "whose inputs changed since the previous draw - same results");
REXCVAR_DEFINE_BOOL(wos_overlay_delta_verify, false, "WoS",
                    "fix104: compute every reusable slot the full way too and log any difference "
                    "([WoS-DELTA])");
extern "C" bool wos_depth_shells() {
  static const bool on = REXCVAR_GET(wos_depth_shells);
  return on;
}
namespace {
constexpr uint32_t kCursor = 48;
constexpr uint32_t kRegCount = 0x5000;  // up to the loop constants (0x4908..)

struct RegMirror {
  uint32_t v[kRegCount];
  uint8_t written[kRegCount];
  // fix35: when each register was last written by the stream, and when D3D's shadow of it
  // last changed (both on one clock: the span serial) - the newer of the two wins a draw
  uint64_t write_serial[kRegCount];
  uint32_t shadow_seen[kRegCount];
  uint64_t shadow_serial[kRegCount];
  uint64_t serial;
  // fix36: the serial of the last stream span the mirror could not read (rejected, or the
  // cursor jumped): a register written after it holds exactly what the GPU has
  uint64_t gap_serial;
  uint64_t last_shadow_change;  // fix62: serial of the newest D3D-shadow change the overlay saw
  // fix104 (wos_overlay_delta): texture slot / vertex stream whose fetch registers got a write
  // that can change the overlay's result for it since it was last computed
  uint32_t tex_dirty, stream_dirty;
  uint32_t im_load[2];       // last IM_LOAD microcode address (0 VS, 1 PS), physical
  uint32_t im_load_size[2];  // dwords
  bool im_immediate[2];
};

struct StreamState {
  uint32_t device = 0, last = 0;
  RegMirror *regs = nullptr;
};
// One stream per device, shared: the main thread and the render job thread (JQ worker 1)
// both draw on the same device (0x0073D080) - a per-thread cursor read the other thread's
// packets as a 2.4 MB span into unmapped memory (the thirteenth run's crash).
std::mutex g_stream_lock;
// fix61: bumped by every mirror write the overlay reads (and by a new mirror)
std::atomic<uint64_t> g_overlay_inputs_epoch{1};
// fix80: performance master switch (wos_fast_capture = false = exactly fix79's behaviour)
extern "C" bool wos_fast_capture_on();
extern "C" bool wos_lean_stream_on();
// WoS port (fix104): the stream mirror's diagnostic counters as plain relaxed load + store, not
// locked read-modify-writes. Every mirror write runs under g_stream_lock; a locked add drains
// the store buffer, and the draw thread made dozens per draw (most of them per packet).
// wos_lean_stream = false brings back the locked adds.
template <class T, class U>
inline void Bump(std::atomic<T> &a, U n) {
  static const bool lean = wos_lean_stream_on();
  if (lean)
    a.store(a.load(std::memory_order_relaxed) + static_cast<T>(n), std::memory_order_relaxed);
  else
    a.fetch_add(static_cast<T>(n), std::memory_order_relaxed);
}
bool OverlayReads(uint32_t reg);  // defined after kStateRegs
std::atomic<uint64_t> g_overlay_memo_hits{0};
StreamState g_streams[4];
StreamState &StreamFor(uint32_t device) {
  StreamState *st = nullptr;
  for (StreamState &s : g_streams)
    if (s.device == device) {
      st = &s;
      break;
    }
  if (!st)
    for (StreamState &s : g_streams)
      if (!s.device) {
        s.device = device;
        st = &s;
        break;
      }
  if (!st)
    st = &g_streams[device % 4];
  if (!st->regs) {
    st->regs = new RegMirror;
    std::memset(st->regs, 0, sizeof(RegMirror));
    g_overlay_inputs_epoch.fetch_add(1, std::memory_order_relaxed);
  }
  return *st;
}

std::atomic<uint64_t> g_fetch_dwords{0}, g_type0_const{0}, g_type0_fetch{0};
std::atomic<uint64_t> g_const_packets{0}, g_const_dwords{0}, g_indirect{0}, g_indirect_draws{0},
    g_skipped{0}, g_parses{0};
// the mirror's own counters
std::atomic<uint64_t> g_reg_writes{0}, g_fetch_writes[2]{}, g_load_alu{0}, g_load_alu_fetch{0},
    g_im_loads{0}, g_set_const2{0}, g_bad_packets{0};
std::atomic<uint64_t> g_overlay_draws{0}, g_overlay_tex_draws{0}, g_overlay_tex_slots{0},
    g_overlay_state_draws{0}, g_overlay_state_regs{0}, g_overlay_rejected{0}, g_shader_mismatch[2]{};
std::atomic<uint64_t> g_verbose_until_overlay{0};
std::atomic<uint64_t> g_overlay_vtx_draws{0}, g_overlay_vtx_streams{0}, g_kicks{0};
std::atomic<uint64_t> g_segment_switches{0};  // fix48
uint64_t g_state_reg_hits[64] = {};  // per entry of kStateRegs (under g_stream_lock)

inline uint32_t Be32(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline void PutBe32(uint8_t *p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

std::atomic<uint64_t> g_const_kept{0};  // fix96
void ApplyConstants(uint32_t device, uint32_t index, uint32_t n, const uint8_t *data,
                    uint64_t mask[2]) {
  if (index >= 2048 || !n)
    return;
  n = std::min(n, 2048 - index);
  uint8_t *dst = eot::mem::at<uint8_t>(device + 0x780 + 4 * index);
  if (!dst)
    return;
  // WoS port (fix96): a ring constant is OLDER than a constant the game set since (D3D's pending
  // mask, device + 0 VS / + 8 PS: SetVertex/PixelShaderConstantF marks the 4-register block):
  // on the console the draw's state flush writes the pending shadow values after every ring
  // packet before it. The mirror applied the ring packets at the draw instead, over the shadow -
  // the previous object's constants overwrote the terrain's freshly set ambient / fog / light
  // registers (c16-c63): the dark-ground and black patches. Pending blocks keep the shadow.
  static const bool keep_pending = REXCVAR_GET(wos_ring_keeps_pending);
  uint64_t pending[2] = {0, 0};
  if (keep_pending)
    if (const uint8_t *pm = eot::mem::at<const uint8_t>(device)) {
      uint64_t v;
      std::memcpy(&v, pm, 8);
      pending[0] = __builtin_bswap64(v);
      std::memcpy(&v, pm + 8, 8);
      pending[1] = __builtin_bswap64(v);
    }
  // fix104: nothing pending to keep (always, with wos_ring_keeps_pending off) - one copy
  static const bool lean = wos_lean_stream_on();
  // WoS port (fix113): wos_fast_apply - the same result a pending-mask block (4 registers = 16
  // dwords, never straddling the VS/PS file boundary at dword 1024) at a time: one test and one
  // copy per block instead of per dword. fix112 measured the per-dword loop at ~38% of the
  // ring flush (~0.28 us a draw) in the city.
  static const bool fast_apply = wos_fast_apply_on();
  if (lean && !pending[0] && !pending[1]) {
    std::memcpy(dst, data, 4u * n);
  } else if (fast_apply) {
    uint32_t i = 0;
    while (i < n) {
      const uint32_t d = index + i;
      const uint32_t run = std::min(n - i, 16u - (d & 15u));
      const uint32_t reg = d / 4;
      const uint32_t file = reg >= 256 ? 1 : 0, block = (reg & 255) / 4;
      if (pending[file] & (1ull << (63 - block)))
        Bump(g_const_kept, run);
      else
        std::memcpy(dst + 4 * i, data + 4 * i, 4u * run);
      i += run;
    }
  } else {
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t reg = (index + i) / 4;
    const uint32_t file = reg >= 256 ? 1 : 0, block = (reg & 255) / 4;
    if (pending[file] & (1ull << (63 - block))) {
      Bump(g_const_kept, 1);
      continue;
    }
    std::memcpy(dst + 4 * i, data + 4 * i, 4);
  }
  }
  if (fast_apply) {
    // the pending bits of every block the packet touched, a block at a time
    for (uint32_t b = index / 16; b <= (index + n - 1) / 16; ++b) {
      const uint32_t reg = b * 4;
      const uint32_t file = reg >= 256 ? 1 : 0, block = (reg & 255) / 4;
      mask[file] |= 1ull << (63 - block);
    }
  } else
  for (uint32_t reg = index / 4; reg <= (index + n - 1) / 4; ++reg) {
    const uint32_t file = reg >= 256 ? 1 : 0, block = (reg & 255) / 4;
    mask[file] |= 1ull << (63 - block);
  }
  Bump(g_const_packets, 1);
  Bump(g_const_dwords, n);
}

// First sight of each indirect buffer: its address, size and what it holds (first 64).
void LogIndirectBuffer(uint32_t addr, uint32_t dwords, const uint8_t *ib, int depth) {
  static std::mutex m;
  static std::vector<uint32_t> seen;
  {
    std::lock_guard lock(m);
    if (seen.size() >= 64 || std::find(seen.begin(), seen.end(), addr) != seen.end())
      return;
    seen.push_back(addr);
  }
  uint32_t t0 = 0, t0_fetch = 0, t0_state = 0, t0_const = 0, nop = 0, draws = 0, setc[5] = {},
           other = 0, loadalu = 0, imload = 0, ibs = 0, bad = 0;
  std::string ops;
  const uint8_t *p = ib, *end = ib + 4u * dwords;
  while (p + 4 <= end) {
    const uint32_t h = Be32(p), type = h >> 30, count = ((h >> 16) & 0x3FFF) + 1;
    if (type == 2) {
      ++nop;
      p += 4;
      continue;
    }
    if (type == 1) {
      ++t0;
      p += 12;
      continue;
    }
    if (p + 4u * (1 + count) > end) {
      ++bad;
      break;
    }
    if (type == 0) {
      ++t0;
      const uint32_t reg = h & 0x7FFF;
      if (reg >= 0x4800 && reg < 0x4900)
        ++t0_fetch;
      else if (reg >= 0x4000 && reg < 0x4800)
        ++t0_const;
      else if (reg >= 0x2000 && reg < 0x2400)
        ++t0_state;
    } else {
      const uint32_t op = (h >> 8) & 0x7F;
      if (op == 0x2D)
        ++setc[std::min<uint32_t>((Be32(p + 4) >> 16) & 0xFF, 4)];
      else if (op == 0x22 || op == 0x36)
        ++draws;
      else if (op == 0x2F)
        ++loadalu;
      else if (op == 0x27 || op == 0x2B)
        ++imload;
      else if (op == 0x3F || op == 0x37)
        ++ibs;
      else if (op == 0x10)
        ++nop;
      else
        ++other;
      if (ops.size() < 96)
        ops += std::format(" {:02x}", op);
    }
    p += 4u * (1 + count);
  }
  std::string head;
  for (uint32_t i = 0; i < std::min<uint32_t>(dwords, 10); ++i)
    head += std::format(" {:08x}", Be32(ib + 4 * i));
  REXLOG_INFO("[WoS-REGS] indirect buffer {:08x} ({} dwords, depth {}): type0 {} (fetch {} const {} "
              "state {}), SET_CONSTANT alu {} fetch {} bool {} loop {} regs {}, LOAD_ALU {}, IM_LOAD {}, "
              "draws {}, IBs {}, other {}, nops {}, bad {} | ops{} | head{}",
              addr, dwords, depth, t0, t0_fetch, t0_const, t0_state, setc[0], setc[1], setc[2],
              setc[3], setc[4], loadalu, imload, draws, ibs, other, nop, bad, ops, head);
}

// fix35: a span is applied only if it is packets end to end - known type-3 opcodes,
// register writes in range, every packet inside the span. The fix34 parser applied
// whatever it met: 9 million "bad packets" a run (one in two draws), the gaps between
// command-stream segments read as packets - garbage textures, depth/cull state, constants.
bool KnownOpcode(uint32_t op) {
  switch (op) {
  case 0x10: case 0x21: case 0x22: case 0x23: case 0x25: case 0x26: case 0x27: case 0x2B:
  case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x34: case 0x35: case 0x36: case 0x37:
  case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x44: case 0x45: case 0x46:
  case 0x48: case 0x4A: case 0x4B: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53:
  case 0x54: case 0x55: case 0x56: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C:
  case 0x5D: case 0x5E: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64:
    return true;
  default:
    return false;
  }
}
std::atomic<uint64_t> g_spans_ok{0}, g_spans_rejected{0}, g_spans_resumed{0};
// Where in [p, end) the packets stop being packets (end when they are packets throughout).
const uint8_t *SpanEnd(const uint8_t *p, const uint8_t *end, uint32_t *bad_header) {
  while (p + 4 <= end) {
    const uint32_t h = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    const uint32_t type = h >> 30;
    if (type == 2) {
      p += 4;
      continue;
    }
    if (type == 1) {
      if (p + 12 > end)
        break;
      p += 12;
      continue;
    }
    const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
    const bool ok = type == 0 ? ((h & 0x7FFF) + ((h >> 15) & 1 ? 1 : count) <= 0x6000)
                              : KnownOpcode((h >> 8) & 0x7F);
    if (!ok || p + 4u * (1 + count) > end) {
      *bad_header = h;
      return p;
    }
    p += 4u * (1 + count);
  }
  return p == end ? end : p;
}

// WoS port (fix112): where the ring flush's ~1.1 us a draw goes - every 8th flush timed in TSC
// ticks by part; logged as [WoS-FLUSH] with [WoS-COST]
struct FlushProf {
  bool on = false;
  uint64_t lock = 0, readable = 0, spanend = 0, parse = 0, apply = 0, masks = 0, total = 0;
  uint64_t samples = 0, bytes = 0, calls = 0, packets = 0, sampled_bytes = 0;
};
FlushProf g_fprof;
inline uint64_t ProfTicks() { return __builtin_ia32_rdtsc(); }

void FakeOcclusionQuery(RegMirror &rm);  // (fix90) below
uint8_t g_flush_before[16 + 2048];  // fix97: pending masks, c0-c63 before / after the flush
// fix94: on a capture frame, the ALU constants the stream carries (0x4000-0x47FF), kept for
// the capture's blob 17 - so an EMULATED capture also has each draw's real VS constants
std::atomic<bool> g_cap_alu_on{false};
uint32_t g_cap_alu[0x800];
void FakeScreenExtent(uint32_t address);  // (fix91) below
// The command processor: the packets in [p, end) into the mirror; with apply_device, ALU
// constants also into the device's shadow (and their pending bits into mask).
void ParsePackets(StreamState &st, const uint8_t *p, const uint8_t *end, int depth,
                  bool apply_device, uint64_t mask[2]) {
  RegMirror &rm = *st.regs;
  const uint32_t device = st.device;
  auto write = [&](uint32_t reg, uint32_t value) {
    if (reg < kRegCount) {
      // fix61/62: a write that can change what the overlay produces for texture fetch slots
      // (0x4800-0x485F) or render state invalidates its memo. D3D rewrites most of them with the
      // same value every draw: such a rewrite changes nothing when the register already won
      // (written after the last gap and after any shadow change). Plain load/store: every
      // mirror write happens under g_stream_lock.
      // fix80 (wos_fast_capture): only the registers the overlay reads (texture fetch slots,
      // kStateRegs) - fix79 also bumped on 0x2100-0x23FF writes it never reads, such as
      // VGT_MAX/MIN_VTX_INDX and VGT_INDX_OFFSET, rewritten for almost every draw (memo hit
      // only ~1/3 of draws). And a same-value rewrite is compared against ITS group's shadow
      // change (what stream_newer reads), not the newest shadow change of any register.
      static const bool fast = wos_fast_capture_on();
      if (fast) {
        if (OverlayReads(reg)) {
          const uint32_t base = reg >= 0x4800 ? 0x4800 + (reg - 0x4800) / 6 * 6 : reg;
          const bool same = rm.written[reg] && rm.v[reg] == value &&
                            rm.write_serial[reg] > rm.gap_serial &&
                            rm.write_serial[reg] > rm.shadow_serial[base];
          if (!same)
            g_overlay_inputs_epoch.store(g_overlay_inputs_epoch.load(std::memory_order_relaxed) + 1,
                                         std::memory_order_relaxed);
        }
      } else if ((reg >= 0x4800 && reg < 0x4860) || (reg >= 0x2100 && reg < 0x2400)) {
        const bool same = rm.written[reg] && rm.v[reg] == value &&
                          rm.write_serial[reg] > rm.gap_serial &&
                          rm.write_serial[reg] > rm.last_shadow_change;
        if (!same)
          g_overlay_inputs_epoch.store(g_overlay_inputs_epoch.load(std::memory_order_relaxed) + 1,
                                       std::memory_order_relaxed);
      }
      // fix104: per-slot / per-stream dirty bits for the overlay delta. A rewrite of the same value
      // after the slot's last shadow change and the last gap cannot change the overlay's result
      // (its stream_newer stays true, its value is the same); any other write can.
      if (reg >= 0x4800 && reg < 0x48C0) {
        const uint32_t off = reg - 0x4800;
        const bool tex = off < 0x60;
        const uint32_t base = tex ? 0x4800 + off / 6 * 6 : 0x4800 + (off & ~1u);
        const bool same = rm.written[reg] && rm.v[reg] == value &&
                          rm.write_serial[reg] > rm.gap_serial &&
                          rm.write_serial[reg] > rm.shadow_serial[base];
        if (!same) {
          if (tex)
            rm.tex_dirty |= 1u << (off / 6);
          else if (off >= 0xA0)  // fetch index 80-95: vertex streams 15..0
            rm.stream_dirty |= 1u << (95 - off / 2);
        }
      }
      rm.v[reg] = value;
      rm.written[reg] = 1;
      rm.write_serial[reg] = rm.serial;
    }
  };
  auto write_range = [&](uint32_t first, const uint8_t *data, uint32_t n) {
    // fix41: ALU constants (0x4000-0x47FF) are never read from the mirror (the overlay uses
    // fetch constants and render state; constants go to the device below) - storing them
    // was most of the overlay's cost: [WoS-COST] measured the overlay at 50% of the D3D
    // thread's wall time, ~730M constant dwords a run through three stores each
    if (g_cap_alu_on.load(std::memory_order_relaxed) && first < 0x4800 && first + n > 0x4000) {
      const uint32_t lo = std::max(first, 0x4000u), hi = std::min(first + n, 0x4800u);
      for (uint32_t r = lo; r < hi; ++r)
        g_cap_alu[r - 0x4000] = Be32(data + 4 * (r - first));
    }
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t reg = first + i;
      if (reg >= 0x4000 && reg < 0x4800) {
        i += std::min(n - i, 0x4800 - reg) - 1;  // skip the rest of the constant block
        continue;
      }
      write(reg, Be32(data + 4 * i));
    }
    Bump(g_reg_writes, n);
    if (first < 0x4900 && first + n > 0x4800)
      Bump(g_fetch_writes[depth ? 1 : 0], 1);
    // ALU constants (0x4000-0x47FF) also into the shadow the renderer uploads from
    if (apply_device && first < 0x4800 && first + n > 0x4000) {
      const uint32_t lo = std::max(first, 0x4000u), hi = std::min(first + n, 0x4800u);
      const uint64_t ta = g_fprof.on ? ProfTicks() : 0;
      ApplyConstants(device, lo - 0x4000, hi - lo, data + 4 * (lo - first), mask);
      if (g_fprof.on)
        g_fprof.apply += ProfTicks() - ta;
    }
  };
  static const bool type0_constants = REXCVAR_GET(wos_stream_type0_constants);
  while (p + 4 <= end) {
    if (g_fprof.on)
      ++g_fprof.packets;
    const uint32_t h = Be32(p);
    const uint32_t type = h >> 30;
    if (type == 2) {
      p += 4;
      continue;
    }
    if (type == 1) {
      if (p + 12 > end)
        break;
      write(h & 0x7FF, Be32(p + 4));
      write((h >> 11) & 0x7FF, Be32(p + 8));
      p += 12;
      continue;
    }
    const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
    if (p + 4u * (1 + count) > end) {
      Bump(g_bad_packets, 1);
      break;
    }
    const uint8_t *d = p + 4;
    if (type == 0) {
      const uint32_t reg = h & 0x7FFF;
      if ((h >> 15) & 1) {  // all to one register
        for (uint32_t i = 0; i < count; ++i)
          write(reg, Be32(d + 4 * i));
      } else if (type0_constants || !(reg < 0x4800 && reg + count > 0x4000)) {
        write_range(reg, d, count);
        if (reg < 0x4800 && reg + count > 0x4000)
          Bump(g_type0_const, count);
      }
      if (reg < 0x4900 && reg + count > 0x4800)
        Bump(g_type0_fetch, count);
    } else {
      const uint32_t op = (h >> 8) & 0x7F;
      switch (op) {
      case 0x2D:  // SET_CONSTANT: index, kind (0 ALU 1 FETCH 2 BOOL 3 LOOP 4 REGISTERS)
        if (count >= 2) {
          static constexpr uint32_t kBase[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
          const uint32_t off = Be32(d), kind = (off >> 16) & 0xFF;
          if (kind <= 4) {
            write_range(kBase[kind] + (off & 0x7FF), d + 4, count - 1);
            if (kind == 1)
              Bump(g_fetch_dwords, count - 1);
          }
        }
        break;
      case 0x55:  // SET_CONSTANT2
      case 0x56:  // SET_SHADER_CONSTANTS
        if (count >= 2) {
          write_range(Be32(d) & 0xFFFF, d + 4, count - 1);
          Bump(g_set_const2, 1);
        }
        break;
      case 0x2F:  // LOAD_ALU_CONSTANT: address, index|kind, size (from memory)
        if (count >= 3) {
          static constexpr uint32_t kBase[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
          const uint32_t addr = Be32(d) & 0x3FFFFFFF, off = Be32(d + 4), kind = (off >> 16) & 0xFF;
          const uint32_t n = Be32(d + 8) & 0xFFF;
          Bump(g_load_alu, 1);
          if (kind == 1)
            Bump(g_load_alu_fetch, 1);
          if (kind <= 4 && n && (kind != 0 || apply_device || g_cap_alu_on.load(std::memory_order_relaxed)) && eot::mem::gpu_readable(addr, 4 * n))
            if (const uint8_t *src = eot::mem::gpu<const uint8_t>(addr))
              write_range(kBase[kind] + (off & 0x7FF), src, n);
        }
        break;
      case 0x27:  // IM_LOAD: address | type, start << 16 | size
        if (count >= 2) {
          const uint32_t at = Be32(d), t = at & 3;
          if (t < 2) {
            rm.im_load[t] = at & ~3u;
            rm.im_load_size[t] = Be32(d + 4) & 0xFFFF;
            rm.im_immediate[t] = false;
          }
          Bump(g_im_loads, 1);
        }
        break;
      case 0x2B:  // IM_LOAD_IMMEDIATE: type, start << 16 | size, code
        if (count >= 2) {
          const uint32_t t = Be32(d) & 3;
          if (t < 2) {
            rm.im_load[t] = 0;
            rm.im_load_size[t] = Be32(d + 4) & 0xFFFF;
            rm.im_immediate[t] = true;
          }
          Bump(g_im_loads, 1);
        }
        break;
      case 0x3F:  // INDIRECT_BUFFER
      case 0x37:  // INDIRECT_BUFFER_PFD
        if (count >= 2) {
          const uint32_t addr = Be32(d) & ~3u, dwords = Be32(d + 4) & 0xFFFFF;
          if (depth == 0)
            Bump(g_indirect, 1);
          if (depth < 2 && dwords && dwords < (1u << 18) && eot::mem::gpu_readable(addr, 4u * dwords)) {
            if (const uint8_t *ib = eot::mem::gpu<const uint8_t>(addr)) {
              wos::capture::NoteBlob(6, addr, ib, 4u * dwords);  // capture: the buffer's packets
              if (REXCVAR_GET(wos_regs_log))
                LogIndirectBuffer(addr, dwords, ib, depth + 1);
              uint32_t bad = 0;
              if (SpanEnd(ib, ib + 4u * dwords, &bad) == ib + 4u * dwords)
                ParsePackets(st, ib, ib + 4u * dwords, depth + 1, apply_device, mask);
              else
                Bump(g_spans_rejected, 1);
            }
          }
        }
        break;
      case 0x22:  // DRAW_INDX
      case 0x36:  // DRAW_INDX_2
        if (depth)
          Bump(g_indirect_draws, 1);
        break;
      case 0x5B:  // EVENT_WRITE_ZPD - an occlusion query's begin / end
        FakeOcclusionQuery(rm);
        break;
      case 0x5A:  // EVENT_WRITE_EXT - screen extent of the previous draws (fix91)
        if (count >= 2)
          FakeScreenExtent(Be32(d + 4));
        break;
      case 0x46:  // EVENT_WRITE: count ZPASS_DONE (21) as a query too
        if (count >= 1 && (Be32(d) & 0x3F) == 21)
          FakeOcclusionQuery(rm);
        break;
      default:
        break;
      }
    }
    p += 4u * (1 + count);
  }
}

// [stream st.last, cursor) into the mirror (apply_device: constants to the shadow too).
// Returns false when the span is unusable (the cursor moved backwards / jumped segments).
// fix36: D3D writes the stream with stwu (store, pre-increment): device + 48 points at the
// LAST word written, not the next free one. The packets since the previous read are
// (last, cur] = [last + 4, cur + 4). Every parser before fix36 read [last, cur): one word
// early - each span began with the previous packet's last word taken for a header (the
// fix34 "9 million bad packets", fix35 rejecting 5,428 of 5,429 spans; shifted by one word,
// 5,421 of them parse cleanly in the fix35 capture) and lost its own last word.
// limit (BeginVertices' draws): read only up to that address - the vertex data D3D reserved
// in the stream starts there - and resume after it later (SkipStreamCursor).
bool SyncStream(StreamState &st, uint32_t device, bool apply_device, uint64_t mask[2],
                uint32_t limit = 0) {
  const uint8_t *dev = eot::mem::at<const uint8_t>(device);
  if (!dev)
    return false;
  uint32_t cur = Be32(dev + kCursor);
  if (limit && st.last && limit > st.last + 4 && limit - 4 <= cur)
    cur = limit - 4;
  const bool prof = g_fprof.on;
  uint64_t pt = prof ? ProfTicks() : 0;
  const bool span_readable = st.last && cur >= st.last && cur - st.last <= (256u << 10) &&
                             eot::mem::readable(st.last + 4, cur - st.last);
  if (prof) {
    const uint64_t t = ProfTicks();
    g_fprof.readable += t - pt;
    pt = t;
  }
  if (apply_device && span_readable) {
    g_fprof.bytes += cur - st.last;
    ++g_fprof.calls;
    if (prof)
      g_fprof.sampled_bytes += cur - st.last;
  }
  if (!span_readable) {
    if (st.last && cur != st.last) {
      Bump(g_skipped, 1);
      if (st.regs)
        st.regs->gap_serial = ++st.regs->serial;
    }
    st.last = cur;
    return false;
  }
  if (cur > st.last)
    if (const uint8_t *from = eot::mem::at<const uint8_t>(st.last + 4)) {
      const uint8_t *to = from + (cur - st.last);
      if (wos::capture::Active()) {  // capture: the raw stream this parse read (kind 10)
        static std::atomic<uint32_t> serial{0};
        wos::capture::NoteBlob(10, serial.fetch_add(1, std::memory_order_relaxed), from,
                               std::min<uint32_t>(cur - st.last, 64u << 10));
      }
      uint32_t bad = 0;
      const uint8_t *stop = SpanEnd(from, to, &bad);
      if (prof) {
        const uint64_t t = ProfTicks();
        g_fprof.spanend += t - pt;
        pt = t;
      }
      if (stop != to) {
        // fix43: not packets throughout - D3D reserved data (vertices, constant blocks: floats
        // like 3f800000) inside the span. fix36-42 dropped the whole span (up to 240 KB, ~2 a
        // frame), losing the game's constants and fetch constants in it: one-frame
        // "discombobulated" skinned meshes and wrong UI textures. Now the packets before the
        // data, and the tail after it: the first offset from which packets run cleanly to the
        // span's end (at least 3 of them; in the fix42 capture the tails held 10-567 packets).
        // fix44: off by default - in the fix43 run whole frames drew from another camera
        // (constants from a false "tail" written to the device). wos_span_tails = true
        // brings it back for experiments.
        Bump(g_spans_rejected, 1);
        st.regs->gap_serial = ++st.regs->serial;
        static const bool tails = REXCVAR_GET(wos_span_tails);
        if (tails && stop > from)
          ParsePackets(st, from, stop, 0, apply_device, mask);
        const uint8_t *resume = nullptr;
        uint32_t budget = tails ? 1u << 19 : 0u;  // packet steps (zero-filled data parses as packets)
        for (const uint8_t *q = stop + 4; q + 4 <= to && budget; q += 4) {
          if (Be32(q) == 0)
            continue;
          uint32_t packets = 0;
          const uint8_t *p = q;
          while (p + 4 <= to && budget) {  // SpanEnd, counting
            --budget;
            const uint32_t h = Be32(p), type = h >> 30;
            uint32_t len;
            if (type == 2) {
              len = 4;
            } else if (type == 1) {
              len = 12;
            } else {
              const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
              const bool ok = type == 0 ? ((h & 0x7FFF) + ((h >> 15) & 1 ? 1 : count) <= 0x6000)
                                        : KnownOpcode((h >> 8) & 0x7F);
              if (!ok)
                break;
              len = 4u * (1 + count);
            }
            if (p + len > to)
              break;
            p += len;
            ++packets;
          }
          if (p == to && packets >= 3) {
            resume = q;
            break;
          }
        }
        if (resume) {
          ParsePackets(st, resume, to, 0, apply_device, mask);
          Bump(g_spans_resumed, 1);
        }
        static std::atomic<uint32_t> logged{0};
        if (REXCVAR_GET(wos_regs_log) && logged.fetch_add(1, std::memory_order_relaxed) < 24)
          REXLOG_INFO("[WoS-REGS] stream span {:08x}..{:08x} ({} bytes): not a packet at +{} (header "
                      "{:08x}); packets resumed at {}",
                      st.last + 4, cur + 4, cur - st.last, stop - from, bad,
                      resume ? std::format("+{}", resume - from) : std::string("- (none)"));
      } else {
        Bump(g_spans_ok, 1);
        ++st.regs->serial;
        ParsePackets(st, from, to, 0, apply_device, mask);
        if (prof)
          g_fprof.parse += ProfTicks() - pt;
      }
    }
  st.last = cur;
  return true;
}

// The render state a material sets, by GPU register, and where D3D's device keeps its
// shadow (the device's register blocks: 0x2000 at + 0x2880, 0x2100 at + 0x28CC, 0x2180 at
// + 0x2920, 0x2200 at + 0x2934, 0x2300 at + 0x29B8, 0x2380 at + 0x2A50; checked against
// RB_COLOR_MASK + 0x28DC, RB_DEPTHCONTROL + 0x2934, PA_CL_VTE_CNTL + 0x294C). Surface,
// viewport, scissor and window registers are left alone: D3D writes those per tile.
struct StateReg {
  uint32_t reg, offset;
  const char *name;
};
constexpr StateReg kStateRegs[] = {
    {0x2104, 0x28DC, "RB_COLOR_MASK"},     {0x2105, 0x28E0, "RB_BLEND_RED"},
    {0x2106, 0x28E4, "RB_BLEND_GREEN"},    {0x2107, 0x28E8, "RB_BLEND_BLUE"},
    {0x2108, 0x28EC, "RB_BLEND_ALPHA"},    {0x210C, 0x28FC, "RB_STENCILREFMASK_BF"},
    {0x210D, 0x2900, "RB_STENCILREFMASK"}, {0x210E, 0x2904, "RB_ALPHA_REF"},
    {0x2200, 0x2934, "RB_DEPTHCONTROL"},   {0x2201, 0x2938, "RB_BLENDCONTROL0"},
    {0x2202, 0x293C, "RB_COLORCONTROL"},   {0x2205, 0x2948, "PA_SU_SC_MODE_CNTL"},
    {0x2209, 0x2958, "RB_BLENDCONTROL1"},  {0x220A, 0x295C, "RB_BLENDCONTROL2"},
    {0x220B, 0x2960, "RB_BLENDCONTROL3"},  {0x2380, 0x2A50, "PA_SU_POLY_OFFSET_FRONT_SCALE"},
    {0x2381, 0x2A54, "PA_SU_POLY_OFFSET_FRONT_OFFSET"},
    {0x2382, 0x2A58, "PA_SU_POLY_OFFSET_BACK_SCALE"},
    {0x2383, 0x2A5C, "PA_SU_POLY_OFFSET_BACK_OFFSET"},
};

bool OverlayReadsScan(uint32_t reg) {
  if (reg >= 0x4800 && reg < 0x4860)
    return true;
  for (const StateReg &sr : kStateRegs)
    if (sr.reg == reg)
      return true;
  return false;
}
// fix104: a table instead of a scan of kStateRegs for every register the stream writes
bool OverlayReads(uint32_t reg) {
  static const std::vector<uint8_t> table = [] {
    std::vector<uint8_t> t(kRegCount);
    for (uint32_t r = 0; r < kRegCount; ++r)
      t[r] = OverlayReadsScan(r);
    return t;
  }();
  static const bool lean = wos_lean_stream_on();
  if (!lean)
    return OverlayReadsScan(reg);
  return reg < kRegCount && table[reg];
}

// fix90: occlusion queries. With no GPU, nothing ever writes a query's sample counts: D3D
// marks the end struct 0xFFFFFEED and waits for the GPU to overwrite it, so every query stays
// pending (or reads as 0 pixels) and the game culls whatever it tests - the dark ground patches
// next to buildings and whole street blocks vanished from the main view (the void) while their
// shadow-map draws stayed. As the emulator does (query_occlusion_fake_sample_count): on the
// ZPD event, clear the counts at RB_SAMPLE_COUNT_ADDR and, for a query's end, report
// wos_occlusion_fake_samples visible samples.
std::atomic<uint64_t> g_zpd_events{0}, g_zpd_ends{0}, g_zpd_unmapped{0};
void FakeOcclusionQuery(RegMirror &rm) {
  Bump(g_zpd_events, 1);
  static const int32_t fake = REXCVAR_GET(wos_occlusion_fake_samples);
  if (fake < 0)
    return;
  const uint32_t addr = rm.v[0x2325];  // RB_SAMPLE_COUNT_ADDR (physical)
  uint8_t *p = addr && eot::mem::gpu_readable(addr, 32) ? eot::mem::gpu<uint8_t>(addr) : nullptr;
  if (!p) {
    g_zpd_unmapped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The struct is little-endian (D3D swaps it); D3D's 0xFFFFFEED marker reads 0xEDFEFFFF here
  uint32_t w[8];
  std::memcpy(w, p, sizeof w);
  constexpr uint32_t kFinished = 0xEDFEFFFFu;
  const bool end = w[4] == kFinished || w[5] == kFinished || w[2] == kFinished || w[3] == kFinished;
  std::memset(w, 0, sizeof w);
  if (end) {
    w[0] = static_cast<uint32_t>(fake);  // Total_A
    w[4] = static_cast<uint32_t>(fake);  // ZPass_A
    Bump(g_zpd_ends, 1);
  }
  std::memcpy(p, w, sizeof w);
}

// fix91: screen extents. D3D asks the GPU, ~400 times a frame, for the screen rectangle the
// preceding draws touched (EVENT_WRITE_EXT: the GPU writes min/max x, y, z there). The game
// uses the answer for visibility: with no GPU it stayed empty and the next frame's main view
// dropped those objects (the void ground patches, street blocks) - their shadow draws, which
// are not tested, stayed. Write what the emulator writes: the whole 8192x8192 range, z 0..1
// (6 x u16, k8in16 at address & 3), wos_fake_screen_extents = false: leave memory untouched.
std::atomic<uint64_t> g_ext_events{0}, g_ext_unmapped{0};
void FakeScreenExtent(uint32_t address) {
  Bump(g_ext_events, 1);
  static const bool on = REXCVAR_GET(wos_fake_screen_extents);
  if (!on)
    return;
  const uint32_t addr = address & ~3u;
  uint8_t *p = addr && eot::mem::gpu_readable(addr, 12) ? eot::mem::gpu<uint8_t>(addr) : nullptr;
  if (!p) {
    g_ext_unmapped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // xenos::kTexture2DCubeMaxWidthHeight (8192) >> 3, stored byte-swapped per 16 bits as the
  // emulator's copy_and_swap_16 does (D3D's address carries k8in16)
  static constexpr uint16_t kExtents[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
  for (int i = 0; i < 6; ++i) {
    p[2 * i] = static_cast<uint8_t>(kExtents[i] >> 8);
    p[2 * i + 1] = static_cast<uint8_t>(kExtents[i]);
  }
}

// A texture fetch constant the renderer can use: type 2, a known format, a base, sane size.
bool UsableTextureFetch(const uint32_t f[6]) {
  if ((f[0] & 3) != 2)
    return false;
  const uint32_t fmt = f[1] & 0x3F, base = f[1] & 0xFFFFF000u;
  if (!base || fmt < 2 || fmt > 0x3A)
    return false;
  const uint32_t dim = (f[5] >> 9) & 3;
  // (a 1D texture's width is 24 bits: the fix34 crash was a "1D" garbage fetch 2^20 wide)
  const uint32_t w = dim == 0 ? (f[2] & 0xFFFFFF) + 1 : (f[2] & 0x1FFF) + 1;
  const uint32_t h = dim == 0 ? 1 : ((f[2] >> 13) & 0x1FFF) + 1;
  if (w > 8192 || h > 8192 || dim == 0)
    return false;
  return eot::mem::gpu_readable(base, 256);
}

std::string FetchText(const uint32_t f[6]) {
  const uint32_t dim = (f[5] >> 9) & 3;
  return std::format("fmt {} {}x{} dim {} base {:08x} tiled {} mips {:08x} [{:08x} {:08x} {:08x} "
                     "{:08x} {:08x} {:08x}]",
                     f[1] & 0x3F, (f[2] & 0x1FFF) + 1, dim == 0 ? 1 : ((f[2] >> 13) & 0x1FFF) + 1, dim,
                     f[1] & 0xFFFFF000u, f[0] >> 31, f[5] & 0xFFFFF000u, f[0], f[1], f[2], f[3], f[4],
                     f[5]);
}
}  // namespace

namespace wos::native_renderer {
// Before a draw: the packets since the previous draw on this device's stream (the game's
// own, between its D3D calls): into the mirror, and ALU constants into the shadow.
std::atomic<uint64_t> g_ns_flush{0}, g_ns_overlay{0}, g_ns_overlay_sync{0};
struct NsScope {
  std::atomic<uint64_t> &acc;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  ~NsScope() {
    Bump(acc, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  }
};
void FlushRingConstants(uint32_t device) {
  if (!device || !eot::mem::at<const uint8_t>(device))
    return;
  NsScope ns_scope{g_ns_flush};
  static uint64_t flush_calls = 0;
  const bool prof = (++flush_calls & 7) == 0;
  const uint64_t pt0 = prof ? ProfTicks() : 0;
  std::lock_guard lock(g_stream_lock);
  uint64_t pt1 = 0;
  if (prof) {
    pt1 = ProfTicks();
    g_fprof.lock += pt1 - pt0;
    g_fprof.on = true;
    ++g_fprof.samples;
  }
  g_cap_alu_on.store(wos::capture::Active(), std::memory_order_relaxed);  // fix94
  // fix97: on a capture frame keep D3D's pending masks and VS c0-c63 before this flush
  const bool cap97 = wos::capture::Active();
  if (cap97)
    std::memcpy(g_flush_before, eot::mem::at<const uint8_t>(device), 16),
        std::memcpy(g_flush_before + 16, eot::mem::at<const uint8_t>(device + 0x780), 1024);
  StreamState &st = StreamFor(device);
  uint64_t mask[2] = {0, 0};
  SyncStream(st, device, true, mask);
  if (cap97)
    std::memcpy(g_flush_before + 16 + 1024, eot::mem::at<const uint8_t>(device + 0x780), 1024);
  const uint64_t pt2 = prof ? ProfTicks() : 0;
  for (int f = 0; f < 2; ++f) {
    if (!mask[f])
      continue;
    uint8_t *m = eot::mem::at<uint8_t>(device + 8 * f);
    uint64_t v;
    std::memcpy(&v, m, 8);
    v = __builtin_bswap64(__builtin_bswap64(v) | mask[f]);
    std::memcpy(m, &v, 8);
  }
  if (prof) {
    const uint64_t t = ProfTicks();
    g_fprof.masks += t - pt2;
    g_fprof.total += t - pt0;
    g_fprof.on = false;
  }
  const uint64_t n = g_parses.load(std::memory_order_relaxed) + 1;
  g_parses.store(n, std::memory_order_relaxed);
  if (n % 20000 == 0) {
    // fix40: what the mirror costs the game's D3D thread (host time, cumulative)
    static auto t_last = std::chrono::steady_clock::now();
    static uint64_t f_last = 0, o_last = 0;
    const auto t_now = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double, std::milli>(t_now - t_last).count();
    const uint64_t f = g_ns_flush.load(), o = g_ns_overlay.load(), os = g_ns_overlay_sync.load();
    static uint64_t os_last = 0;
    REXLOG_INFO("[WoS-COST] last {:.0f} ms: stream flush {:.1f} ms, overlay {:.1f} ms (its stream "
                "sync {:.1f} ms) ({:.1f}% of wall) | overlay memo hits {} of {} draws (fix61)",
                wall, (f - f_last) / 1e6, (o - o_last) / 1e6, (os - os_last) / 1e6,
                wall > 0 ? 100.0 * ((f - f_last) + (o - o_last)) / 1e6 / wall : 0.0,
                g_overlay_memo_hits.load(), g_overlay_draws.load());
    os_last = os;
    t_last = t_now;
    f_last = f;
    o_last = o;
    {
      FlushProf &fp = g_fprof;
      const double tot = fp.total ? static_cast<double>(fp.total) : 1.0;
      const double other = static_cast<double>(fp.total) -
                           static_cast<double>(fp.lock + fp.readable + fp.spanend + fp.parse + fp.masks);
      REXLOG_INFO("[WoS-FLUSH] fix112 ({} sampled flushes): lock {:.0f}% readable {:.0f}% span-check "
                  "{:.0f}% parse {:.0f}% (constants into the shadow {:.0f}%) masks {:.0f}% other "
                  "{:.0f}% | {:.0f} ticks a flush | {:.0f} bytes, {:.1f} packets a flush",
                  fp.samples, 100.0 * fp.lock / tot, 100.0 * fp.readable / tot, 100.0 * fp.spanend / tot,
                  100.0 * fp.parse / tot, 100.0 * fp.apply / tot, 100.0 * fp.masks / tot,
                  100.0 * other / tot, fp.samples ? tot / fp.samples : 0.0,
                  fp.calls ? static_cast<double>(fp.bytes) / fp.calls : 0.0,
                  fp.samples ? static_cast<double>(fp.packets) / fp.samples : 0.0);
      fp = FlushProf{};
    }
    REXLOG_INFO("[WoS-RENDERER] stream constants: {} packets, {} dwords applied | {} indirect "
                "buffers ({} draws inside) | {} gaps (stream jumped) | {} fetch SET_CONSTANT dwords | "
                "type0: {} constant dwords, {} fetch dwords",
                g_const_packets.load(), g_const_dwords.load(), g_indirect.load(),
                g_indirect_draws.load(), g_skipped.load(), g_fetch_dwords.load(), g_type0_const.load(),
                g_type0_fetch.load());
    REXLOG_INFO("[WoS-CONST] ring constant dwords kept from the shadow (pending): {}",
                g_const_kept.load());
    REXLOG_INFO("[WoS-ZPD] occlusion query events {} (ends {}, unmapped address {}) | screen "
                "extent events {} (unmapped {})",
                g_zpd_events.load(), g_zpd_ends.load(), g_zpd_unmapped.load(), g_ext_events.load(),
                g_ext_unmapped.load());
    if (REXCVAR_GET(wos_regs_log)) {
      REXLOG_INFO("[WoS-REGS] mirror: {} register writes, fetch writes {} (stream) {} (in IBs), "
                  "LOAD_ALU {} ({} fetch), SET_CONSTANT2/SHADER {}, IM_LOAD {}, bad packets {}, spans "
                  "ok {} rejected {} (tail resumed {}), kicks {} (segment switches {}) | "
                  "draws {}: textures replaced in {} ({} slots), vertex buffers in {} ({} streams), "
                  "state in {} ({} regs), rejected fetches {}, shader differs vs {} ps {}",
                  g_reg_writes.load(), g_fetch_writes[0].load(), g_fetch_writes[1].load(),
                  g_load_alu.load(), g_load_alu_fetch.load(), g_set_const2.load(), g_im_loads.load(),
                  g_bad_packets.load(), g_spans_ok.load(), g_spans_rejected.load(), g_spans_resumed.load(), g_kicks.load(), g_segment_switches.load(),
                  g_overlay_draws.load(), g_overlay_tex_draws.load(),
                  g_overlay_tex_slots.load(), g_overlay_vtx_draws.load(), g_overlay_vtx_streams.load(),
                  g_overlay_state_draws.load(),
                  g_overlay_state_regs.load(), g_overlay_rejected.load(), g_shader_mismatch[0].load(),
                  g_shader_mismatch[1].load());
      std::string regs;
      for (size_t i = 0; i < std::size(kStateRegs); ++i)
        if (g_state_reg_hits[i])
          regs += std::format(" {} {}", kStateRegs[i].name, g_state_reg_hits[i]);
      if (!regs.empty())
        REXLOG_INFO("[WoS-REGS] render state taken from the stream:{}", regs);
    }
  }
}
// After a draw: its own packets (D3D's flush, then the DRAW packet) into the mirror only -
// their constants are D3D's shadow already.
void NoteStreamCursor(uint32_t device) {
  if (!device)
    return;
  std::lock_guard lock(g_stream_lock);
  StreamState &st = StreamFor(device);
  uint64_t mask[2] = {0, 0};
  SyncStream(st, device, false, mask);
}
// After BeginVertices: the stream up to the cursor ends in the vertex space it reserved,
// which the game fills only now - not packets: skipped unread (as before fix34).
void SkipStreamCursor(uint32_t device) {
  if (!device)
    return;
  std::lock_guard lock(g_stream_lock);
  StreamState &st = StreamFor(device);
  if (const uint8_t *dev = eot::mem::at<const uint8_t>(device))
    st.last = Be32(dev + kCursor);
}

// fix35: D3D's kick (sub_82ABA950: submits the command stream's current segment and moves
// the write cursor into the next). The old segment is read up to the cursor first, and
// reading resumes at the new segment's start - the bytes between two segments are not
// packets (fix34 read them as such whenever the next segment lay above the last).
void BeforeKick(uint32_t device) { FlushRingConstants(device); }
void AfterKick(uint32_t device) {
  SkipStreamCursor(device);
  Bump(g_kicks, 1);
}
// fix48: D3D's segment switch (also reached from its ring reserve, without a kick)
void AfterSegmentSwitch(uint32_t device) {
  SkipStreamCursor(device);
  g_segment_switches.fetch_add(1, std::memory_order_relaxed);
}

// Log every overlay for the next n draws (the pad-triggered diagnostic frame).
void VerboseOverlays(uint64_t n) {
  g_verbose_until_overlay.store(g_overlay_draws.load(std::memory_order_relaxed) + n,
                                std::memory_order_relaxed);
}
}  // namespace wos::native_renderer

// Called by Edge of Time's CaptureDraw (gpu/draw.cpp) with the draw's device window, after
// the game's own D3D draw flushed its state into the stream: bring the mirror up to the
// stream's cursor and give the window the GPU's texture fetch constants and render state.
// sync = 1: up to the cursor; 0: the mirror as it is; any other value (BeginVertices'
// draws): up to that address, where the vertex data D3D reserved in the stream begins.
namespace wos::native_renderer {
// fix46b/fix49: the calling guest thread's call stack - ctx.lr, then each frame's saved LR
// ([back chain] - 8) - as up to 16 big-endian words; every read stays inside the thread's own
// stack [limit, base) (fix46 read one word past the base and crashed). Returns the count.
}  // namespace wos::native_renderer
// WoS port (fix114): for the thread sampler's guest stack walk (another thread's stack: checked)
extern "C" bool wos_guest_be32(uint32_t va, uint32_t *out) {
  if (!eot::mem::readable(va, 4))
    return false;
  uint32_t v;
  std::memcpy(&v, eot::mem::at<const uint8_t>(va), 4);
  *out = __builtin_bswap32(v);
  return true;
}
namespace wos::native_renderer {
uint32_t GuestCallStack(uint8_t out[16 * 4]) {
  uint32_t stack_limit = 0, stack_base = 0;
  const PPCContext *c = wos::threads::CurrentContext();
  if (!c || !wos::threads::CurrentStackBounds(&stack_limit, &stack_base))
    return 0;
  uint32_t n = 0;
  PutBe32(out, uint32_t(c->lr));
  ++n;
  uint32_t sp = c->r1.u32;
  while (n < 16 && sp >= stack_limit && sp + 4 <= stack_base) {
    const uint32_t next = Be32(eot::mem::at<const uint8_t>(sp));
    if (next <= sp || next - 8 < stack_limit || next - 8 + 4 > stack_base)
      break;
    PutBe32(out + 4 * n, Be32(eot::mem::at<const uint8_t>(next - 8)));
    ++n;
    sp = next;
  }
  return n;
}
}  // namespace wos::native_renderer

extern "C" void wos_regs_overlay(uint32_t device, uint8_t *image, uint32_t image_bytes, uint32_t sync) {
  if (!device || !image || image_bytes < 0x3140 || !wos::native_renderer::Enabled())
    return;
  const bool textures = REXCVAR_GET(wos_regs_textures), state = REXCVAR_GET(wos_regs_state);
  const bool log = REXCVAR_GET(wos_regs_log);
  wos::native_renderer::NsScope ns_scope{wos::native_renderer::g_ns_overlay};
  std::lock_guard lock(g_stream_lock);
  StreamState &st = StreamFor(device);
  uint64_t mask[2] = {0, 0};
  if (sync) {
    wos::native_renderer::NsScope sync_scope{wos::native_renderer::g_ns_overlay_sync};
    SyncStream(st, device, false, mask, sync == 1 ? 0 : sync);
  }
  RegMirror &rm = *st.regs;
  // fix61: memo. Consecutive draws (the city's ~1000-2800 facade draws a frame) usually change
  // only ALU constants between them: no write to a register the overlay reads, no gap, and the
  // same D3D shadow in the window -> the overlay's result is the previous draw's. Its inputs
  // are the window's fetch block (0x480-0x77F) and render-state words; its outputs the same
  // plus zeroed texture objects. Not used while capturing (blob 9) or logging verbosely.
  struct OverlayMemo {
    bool valid = false;
    uint32_t device = 0;
    uint64_t epoch = 0, gap = 0;
    bool textures = false, state = false, vertex = false;
    uint8_t in_fetch[0x180], out_fetch[0x180];
    uint32_t in_state[std::size(kStateRegs)], out_state[std::size(kStateRegs)];
    uint32_t texobj_cleared = 0, tex_slots = 0, vtx_streams = 0, state_regs = 0;
  };
  static OverlayMemo memo;
  const bool vertex_on = REXCVAR_GET(wos_regs_vertex) && image_bytes >= 0x780;
  const uint64_t epoch_now = g_overlay_inputs_epoch.load(std::memory_order_relaxed);
  const bool capturing = wos::capture::Active();
  auto state_in = [&](size_t i) {
    const StateReg &sr = kStateRegs[i];
    return sr.offset + 4 <= image_bytes ? Be32(image + sr.offset) : 0u;
  };
  const bool verbose_next = log && g_overlay_draws.load(std::memory_order_relaxed) <
                                       g_verbose_until_overlay.load(std::memory_order_relaxed);
  // fix62: the vertex streams (fetch 0x700-0x77F) differ between ~half the facade draws, so
  // the memo covers texture slots (0x480-0x5FF) and render state; streams always run
  bool memo_hit = false;
  if (memo.valid && !capturing && !verbose_next && memo.device == device && memo.epoch == epoch_now &&
      memo.gap == rm.gap_serial && memo.textures == textures && memo.state == state &&
      memo.vertex == vertex_on && std::memcmp(memo.in_fetch, image + 0x480, 0x180) == 0) {
    memo_hit = true;
    for (size_t i = 0; i < std::size(kStateRegs) && memo_hit; ++i)
      memo_hit = memo.in_state[i] == state_in(i);
  }
  // fix64: wos_overlay_memo = false disables the memo; wos_overlay_memo_verify computes every
  // memo hit the slow way too and logs any difference (the facade flashing test)
  static const bool memo_on = REXCVAR_GET(wos_overlay_memo);
  static const bool memo_verify = REXCVAR_GET(wos_overlay_memo_verify);
  if (!memo_on)
    memo_hit = false;
  bool memo_check = false;
  uint8_t check_fetch[0x180];
  uint32_t check_state[std::size(kStateRegs)];
  uint32_t check_texobj = 0;
  if (memo_hit && memo_verify) {
    memo_check = true;
    memo_hit = false;
    std::memcpy(check_fetch, memo.out_fetch, sizeof check_fetch);
    std::memcpy(check_state, memo.out_state, sizeof check_state);
    check_texobj = memo.texobj_cleared;
  }
  if (memo_hit) {
    std::memcpy(image + 0x480, memo.out_fetch, 0x180);
    for (size_t i = 0; i < std::size(kStateRegs); ++i)
      if (kStateRegs[i].offset + 4 <= image_bytes)
        PutBe32(image + kStateRegs[i].offset, memo.out_state[i]);
    for (uint32_t slot = 0; slot < 16; ++slot)
      if (memo.texobj_cleared & (1u << slot))
        PutBe32(image + eot::gpu::dev::kTextureObject0 + 4 * slot, 0);
    Bump(g_overlay_memo_hits, 1);
  } else {
    memo.valid = false;
    std::memcpy(memo.in_fetch, image + 0x480, 0x180);
    for (size_t i = 0; i < std::size(kStateRegs); ++i)
      memo.in_state[i] = state_in(i);
    memo.texobj_cleared = 0;
  }
  // Which value of registers [r, r + n) the GPU has. fix36: read at the right boundaries the
  // stream holds D3D's own flushes too, so the mirror IS the GPU's register file - it wins
  // whenever no span was lost since it was written. Only after a gap does fix35's rule
  // decide (the stream wins if it wrote the registers after D3D's shadow of them last
  // changed). Simulated on the fix35 capture: shadow-change ordering alone left 850 state
  // and 3 texture values wrong in a frame; the GPU's own values are right by construction.
  // (fix34 let a misaligned mirror always win: menus, fonts and videos drew white.)
  auto stream_newer = [&](uint32_t r, uint32_t n, const uint8_t *shadow) {
    bool changed = false;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t v = Be32(shadow + 4 * i);
      if (rm.shadow_seen[r + i] != v) {
        rm.shadow_seen[r + i] = v;
        changed = true;
      }
    }
    if (changed) {
      rm.shadow_serial[r] = ++rm.serial;
      rm.last_shadow_change = rm.serial;
    }
    uint64_t ws = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (!rm.written[r + i])
        return false;
      ws = std::max(ws, rm.write_serial[r + i]);
    }
    return ws > rm.gap_serial || ws > rm.shadow_serial[r];
  };
  const uint64_t draw = g_overlay_draws.load(std::memory_order_relaxed);
  g_overlay_draws.store(draw + 1, std::memory_order_relaxed);
  const bool verbose = log && draw < g_verbose_until_overlay.load(std::memory_order_relaxed);
  std::string vline;
  uint32_t tex_slots = memo_hit ? memo.tex_slots : 0, state_regs = memo_hit ? memo.state_regs : 0;
  // fix104 (wos_overlay_delta): per-slot reuse. A slot whose shadow input is byte-identical to
  // the last time it was computed, with no result-changing stream write since (tex_dirty) and
  // no gap, gives the same result: stream_newer's inputs (written, write serials, gap, shadow
  // serial) are unchanged and its shadow bookkeeping is a no-op for unchanged shadow bytes.
  struct OverlayDelta {
    uint32_t device = 0;
    uint64_t gap = ~0ull;
    uint32_t tvalid = 0, trep = 0, vvalid = 0, vrep = 0;
    uint8_t tin[16][24], tout[16][24], vin[16][8], vout[16][8];
  };
  static OverlayDelta dl;
  static const bool delta_on = REXCVAR_GET(wos_overlay_delta);
  static const bool delta_verify = REXCVAR_GET(wos_overlay_delta_verify);
  const bool delta_ok = delta_on && !capturing && !verbose_next && !log;
  if (!delta_ok || dl.device != device || dl.gap != rm.gap_serial) {
    dl.tvalid = dl.vvalid = 0;
    dl.device = device;
    dl.gap = rm.gap_serial;
  }
  auto delta_mismatch = [&](const char *what, uint32_t i, bool pred, bool got) {
    static std::atomic<uint64_t> bad{0};
    const uint64_t b = bad.load(std::memory_order_relaxed) + 1;
    bad.store(b, std::memory_order_relaxed);
    if (b <= 40)
      REXLOG_INFO("[WoS-DELTA] draw {} {} {}: reuse would differ (predicted replaced {}, got {})",
                  draw, what, i, pred, got);
  };
  static std::atomic<uint64_t> delta_checks{0};
  if (textures && !memo_hit) {
    for (uint32_t slot = 0; slot < 16; ++slot) {
      const uint32_t bit = 1u << slot;
      uint8_t *dst = image + 0x480 + 24 * slot;
      bool predicted = false, pred_rep = false;
      uint8_t pred_out[24];
      if (delta_ok && (dl.tvalid & bit) && !(rm.tex_dirty & bit) &&
          std::memcmp(dl.tin[slot], dst, 24) == 0) {
        predicted = true;
        pred_rep = (dl.trep & bit) != 0;
        if (!delta_verify) {
          if (pred_rep) {
            std::memcpy(dst, dl.tout[slot], 24);
            PutBe32(image + eot::gpu::dev::kTextureObject0 + 4 * slot, 0);
            memo.texobj_cleared |= bit;
            ++tex_slots;
          }
          continue;
        }
        std::memcpy(pred_out, dl.tout[slot], 24);
      }
      if (delta_ok)
        std::memcpy(dl.tin[slot], dst, 24);
      auto compute = [&]() -> bool {
          const uint32_t r = 0x4800 + 6 * slot;
          uint8_t *dst = image + 0x480 + 24 * slot;
          if (!stream_newer(r, 6, dst))
            return false;
          uint32_t f[6];
          for (uint32_t i = 0; i < 6; ++i)
            f[i] = rm.v[r + i];
          bool same = true;
          for (uint32_t i = 0; i < 6 && same; ++i)
            same = Be32(dst + 4 * i) == f[i];
          if (same)
            return false;
          if (!UsableTextureFetch(f)) {
            if ((f[0] & 3) == 2)
              Bump(g_overlay_rejected, 1);
            return false;
          }
          uint32_t old[6];
          for (uint32_t i = 0; i < 6; ++i) {
            old[i] = Be32(dst + 4 * i);
            PutBe32(dst + 4 * i, f[i]);
          }
          // the stale texture object (if any) no longer describes this slot
          PutBe32(image + eot::gpu::dev::kTextureObject0 + 4 * slot, 0);
          memo.texobj_cleared |= 1u << slot;
          ++tex_slots;
          static std::atomic<uint32_t> logged{0};
          if (log && logged.fetch_add(1, std::memory_order_relaxed) < 48)
            REXLOG_INFO("[WoS-REGS] draw {} slot {}: stream texture {} (shadow had {})", draw, slot,
                        FetchText(f), FetchText(old));
          if (verbose)
            vline += std::format(" t{}={:x}/{}", slot, f[1] & 0xFFFFF000u, f[1] & 0x3F);
        return true;
      };
      const bool replaced = compute();
      if (delta_ok) {
        if (replaced)
          std::memcpy(dl.tout[slot], dst, 24);
        dl.trep = replaced ? (dl.trep | bit) : (dl.trep & ~bit);
        dl.tvalid |= bit;
        rm.tex_dirty &= ~bit;
        if (predicted) {
          Bump(delta_checks, 1);
          if (pred_rep != replaced || (replaced && std::memcmp(pred_out, dst, 24) != 0))
            delta_mismatch("texture slot", slot, pred_rep, replaced);
        }
      }
    }
  }
  // vertex streams: D3D puts stream n's fetch constant at fetch index 95 - n (2 dwords;
  // the window holds all 32 slots since fix34); the stride stays SetStreamSource's
  uint32_t vtx_streams = 0;
  if (vertex_on) {
    for (uint32_t stream = 0; stream < 16; ++stream) {
      const uint32_t bit = 1u << stream;
      uint8_t *dst = image + 0x480 + 8 * (95 - stream);
      bool predicted = false, pred_rep = false;
      uint8_t pred_out[8];
      if (delta_ok && (dl.vvalid & bit) && !(rm.stream_dirty & bit) &&
          std::memcmp(dl.vin[stream], dst, 8) == 0) {
        predicted = true;
        pred_rep = (dl.vrep & bit) != 0;
        if (!delta_verify) {
          if (pred_rep) {
            std::memcpy(dst, dl.vout[stream], 8);
            ++vtx_streams;
          }
          continue;
        }
        std::memcpy(pred_out, dl.vout[stream], 8);
      }
      if (delta_ok)
        std::memcpy(dl.vin[stream], dst, 8);
      auto compute = [&]() -> bool {
          const uint32_t index = 95 - stream, r = 0x4800 + 2 * index;
          if (!stream_newer(r, 2, dst))
            return false;
          const uint32_t d0 = rm.v[r], d1 = rm.v[r + 1];
          if (Be32(dst) == d0 && Be32(dst + 4) == d1)
            return false;
          const uint32_t addr = d0 & ~3u, bytes = ((d1 >> 2) & 0xFFFFFF) * 4u;
          if ((d0 & 3) != 3 || !addr || !bytes || !eot::mem::gpu_readable(addr, std::min(bytes, 64u)))
            return false;
          const uint32_t old0 = Be32(dst), old1 = Be32(dst + 4);
          PutBe32(dst, d0);
          PutBe32(dst + 4, d1);
          ++vtx_streams;
          static std::atomic<uint32_t> logged{0};
          if (log && logged.fetch_add(1, std::memory_order_relaxed) < 24)
            REXLOG_INFO("[WoS-REGS] draw {} stream {}: vertex buffer {:08x} ({} bytes) from the stream "
                        "(shadow had {:08x} {:08x})",
                        draw, stream, addr, bytes, old0, old1);
          if (verbose)
            vline += std::format(" s{}={:08x}/{}", stream, addr, bytes);
        return true;
      };
      const bool replaced = compute();
      if (delta_ok) {
        if (replaced)
          std::memcpy(dl.vout[stream], dst, 8);
        dl.vrep = replaced ? (dl.vrep | bit) : (dl.vrep & ~bit);
        dl.vvalid |= bit;
        rm.stream_dirty &= ~bit;
        if (predicted) {
          Bump(delta_checks, 1);
          if (pred_rep != replaced || (replaced && std::memcmp(pred_out, dst, 8) != 0))
            delta_mismatch("vertex stream", stream, pred_rep, replaced);
        }
      }
    }
    if (delta_verify) {
      const uint64_t c = delta_checks.load(std::memory_order_relaxed);
      static uint64_t reported = 0;
      if (c >= reported + (1u << 20)) {
        reported = c;
        REXLOG_INFO("[WoS-DELTA] verified {} reused slots/streams (mismatches are logged above)", c);
      }
    }
  }
  if (vtx_streams) {
    Bump(g_overlay_vtx_draws, 1);
    Bump(g_overlay_vtx_streams, vtx_streams);
  }
  if (!memo_hit) {
    for (size_t i = 0; i < std::size(kStateRegs); ++i) {
      const StateReg &sr = kStateRegs[i];
      if (sr.offset + 4 > image_bytes || !stream_newer(sr.reg, 1, image + sr.offset))
        continue;
      const uint32_t v = rm.v[sr.reg], old = Be32(image + sr.offset);
      if (v == old)
        continue;
      if (state)
        PutBe32(image + sr.offset, v);
      ++state_regs;
      ++g_state_reg_hits[i];
      static std::atomic<uint32_t> logged{0};
      if (log && logged.fetch_add(1, std::memory_order_relaxed) < 48)
        REXLOG_INFO("[WoS-REGS] draw {}: {} {:08x} from the stream (shadow had {:08x}){}", draw,
                    sr.name, v, old, state ? "" : " - not applied (wos_regs_state = false)");
      if (verbose)
        vline += std::format(" {}={:08x}(was {:08x})", sr.name, v, old);
    }
  }
  // fix61: remember this result for the next draw with the same inputs
  if (!verbose && !memo_hit) {
    std::memcpy(memo.out_fetch, image + 0x480, 0x180);
    for (size_t i = 0; i < std::size(kStateRegs); ++i)
      memo.out_state[i] = state_in(i);
    memo.device = device;
    memo.epoch = epoch_now;
    memo.gap = rm.gap_serial;
    memo.textures = textures;
    memo.state = state;
    memo.vertex = vertex_on;
    memo.tex_slots = tex_slots;
    memo.vtx_streams = vtx_streams;
    memo.state_regs = state_regs;
    memo.valid = true;
  }
  if (memo_check) {
    uint32_t bad_fetch = 0, bad_state = 0;
    for (uint32_t w = 0; w < 0x180 / 4; ++w)
      bad_fetch += std::memcmp(check_fetch + 4 * w, image + 0x480 + 4 * w, 4) != 0;
    for (size_t i = 0; i < std::size(kStateRegs); ++i)
      bad_state += check_state[i] != state_in(i);
    const bool bad_obj = check_texobj != memo.texobj_cleared;
    static std::atomic<uint64_t> checks{0}, mismatches{0};
    const uint64_t c = checks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad_fetch || bad_state || bad_obj) {
      const uint64_t m = mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
      if (m <= 40)
        REXLOG_INFO("[WoS-MEMO] draw {}: memo result differs: {} fetch words, {} state regs, "
                    "texture objects {:#x} vs {:#x}",
                    draw, bad_fetch, bad_state, check_texobj, memo.texobj_cleared);
    }
    if ((c & 0x3FFFF) == 0)
      REXLOG_INFO("[WoS-MEMO] verified {} memo hits, {} differed", c, mismatches.load());
  }
  // which shader the stream last loaded vs the device's shader objects (diagnostic only;
  // the shader and target pointers are outside the window: read from the live device)
  // fix61: sampled (every 32nd draw) - it reads the live device and guest objects per draw
  const uint8_t *live = (draw & 31) == 0 || verbose ? eot::mem::at<const uint8_t>(device) : nullptr;
  for (int t = 0; t < 2 && live; ++t) {
    const uint32_t obj = Be32(live + (t ? eot::gpu::dev::kPixelShader : eot::gpu::dev::kVertexShader));
    if (!obj || !rm.im_load[t] || !eot::mem::readable(obj, 0x40))
      continue;
    const uint32_t phys = eot::mem::load<uint32_t>(
        obj + (t ? eot::gpu::obj::kPixelShaderPhysical : eot::gpu::obj::kVertexShaderPhysical));
    const uint32_t a = phys & 0x1FFFFFFFu, b = rm.im_load[t] & 0x1FFFFFFFu;
    if ((a > b ? a - b : b - a) > 0x4000) {  // (the object points a little before its code)
      Bump(g_shader_mismatch[t], 1);
      static std::atomic<uint32_t> logged{0};
      if (log && logged.fetch_add(1, std::memory_order_relaxed) < 24)
        REXLOG_INFO("[WoS-REGS] draw {}: {} object {:08x} has microcode {:08x}, the stream last "
                    "loaded {:08x} ({} dwords)",
                    draw, t ? "ps" : "vs", obj, phys, rm.im_load[t], rm.im_load_size[t]);
      if (verbose)
        vline += std::format(" {}obj={:08x}/{:08x} imload={:08x}", t ? "ps" : "vs", obj, phys,
                             rm.im_load[t]);
    }
  }
  if (wos::capture::Active()) {  // capture: the mirror at this draw (kind 9, key = draw serial)
    // layout: mirror fetch 0x4800-0x48BF (192 dwords), mirror kStateRegs, IM_LOAD vs/ps, then
    // (fix36) what the draw got after the overlay: window fetch slots 0-15 (96 dwords),
    // window kStateRegs, and texture objects 0-15 (16 dwords)
    uint8_t blob[192 * 4 + std::size(kStateRegs) * 4 + 8 + 96 * 4 + std::size(kStateRegs) * 4 + 16 * 4];
    uint8_t *o = blob;
    for (uint32_t r = 0x4800; r < 0x48C0; ++r, o += 4)
      PutBe32(o, rm.written[r] ? rm.v[r] : 0);
    for (const StateReg &sr : kStateRegs) {
      PutBe32(o, rm.written[sr.reg] ? rm.v[sr.reg] : 0);
      o += 4;
    }
    PutBe32(o, rm.im_load[0]);
    PutBe32(o + 4, rm.im_load[1]);
    o += 8;
    std::memcpy(o, image + 0x480, 96 * 4);
    o += 96 * 4;
    for (const StateReg &sr : kStateRegs) {
      std::memcpy(o, image + sr.offset, 4);
      o += 4;
    }
    std::memcpy(o, image + eot::gpu::dev::kTextureObject0, 16 * 4);
    wos::capture::NoteBlob(9, uint32_t(draw), blob, sizeof blob);
    // fix46: the game's call stack of this draw (blob 11, key = draw serial): the draw
    // call's return address, then each frame's saved LR ([back chain] - 8), up to 16, as
    // big-endian words. Finds the code that draws (or skips) a building's detailed mesh.
    {
      uint8_t stack[16 * 4];
      if (const uint32_t n = wos::native_renderer::GuestCallStack(stack))
        wos::capture::NoteBlob(11, uint32_t(draw), stack, 4 * n);
    }
    wos::native_renderer::NoteDrawItem(uint32_t(draw), device);  // fix92/94
    // fix51: the draw's guest r14-r31 (blob 13, key = draw serial). The draw hook runs at the
    // game's call, so these still hold the game's draw loop's values - one of them is the
    // render item, which links each draw to its push (blob 12) and so to the code that
    // pushed it (the buildings' stand-ins vs their missing medium LOD).
    if (const PPCContext *c = wos::threads::CurrentContext()) {
      uint8_t regs[18 * 4];
      const PPCRegister *r = &c->r14;
      for (int i = 0; i < 18; ++i)
        PutBe32(regs + 4 * i, r[i].u32);
      wos::capture::NoteBlob(13, uint32_t(draw), regs, sizeof regs);
    }
    // fix37: the guest bytes of every texture this draw samples (blob 1, key = base) - the
    // capture's own blob 1 comes from D3D's shadow, which the world's draws do not use
    for (uint32_t slot = 0; slot < 16; ++slot) {
      const uint8_t *f = image + 0x480 + 24 * slot;
      const uint32_t w0 = Be32(f), w1 = Be32(f + 4), w2 = Be32(f + 8);
      if ((w0 & 3) != 2 || !(w1 & 0xFFFFF000u))
        continue;
      const uint32_t fmt = w1 & 0x3F, w = (w2 & 0x1FFF) + 1, h = ((w2 >> 13) & 0x1FFF) + 1;
      const uint32_t bits = fmt == 18 ? 4 : (fmt == 19 || fmt == 20) ? 8 : fmt == 2 ? 8 : 32;
      uint32_t bytes = std::max<uint32_t>(4096, ((std::max(w, 32u) * std::max(h, 32u) * bits / 8) + 4095) & ~4095u);
      bytes = std::min<uint32_t>(bytes, 1u << 20);
      const uint32_t base = w1 & 0xFFFFF000u;
      while (bytes > 4096 && !eot::mem::gpu_readable(base, bytes))
        bytes >>= 1;
      if (eot::mem::gpu_readable(base, bytes))
        if (const uint8_t *p = eot::mem::gpu<const uint8_t>(base))
          wos::capture::NoteBlob(1, base, p, bytes);
    }
  }
  if (tex_slots) {
    Bump(g_overlay_tex_draws, 1);
    Bump(g_overlay_tex_slots, tex_slots);
  }
  if (state_regs) {
    Bump(g_overlay_state_draws, 1);
    Bump(g_overlay_state_regs, state_regs);
  }
  if (verbose && live)
    REXLOG_INFO("[WoS-REGS] frame draw {}: vs {:08x} ps {:08x} rt0 {:08x} blend {:08x} depth {:08x} "
                "mode {:08x} mask {:08x} |{}",
                draw, Be32(live + eot::gpu::dev::kVertexShader), Be32(live + eot::gpu::dev::kPixelShader),
                Be32(live + eot::gpu::dev::kRenderTarget0), Be32(image + 0x2938), Be32(image + 0x2934),
                Be32(image + 0x2948), Be32(image + 0x28DC), vline);
}


// A world mesh's draw (sub_82914058, object = r3): its VS and PS constants, then
// RunCommandBuffer(device, object + 4) - the mesh's own prebuilt command buffer. Recorded in
// the capture (object, command buffer object, the call) to learn those buffers' layout.
extern "C" void __imp__sub_82914058(PPCContext &ctx, uint8_t *base);
extern "C" void sub_82914058(PPCContext &ctx, uint8_t *base) {
  if (wos::native_renderer::Enabled() && wos::capture::Active()) {
    const uint32_t obj = ctx.r3.u32;
    if (eot::mem::readable(obj, 64)) {
      wos::capture::NoteBlob(7, obj, eot::mem::at<const uint8_t>(obj), 64);
      const uint32_t cb = eot::mem::load<uint32_t>(obj + 4);
      if (cb && eot::mem::readable(cb, 128))
        wos::capture::NoteBlob(8, cb, eot::mem::at<const uint8_t>(cb), 128);
    }
    wos::capture::Call("MeshDraw", ctx, base, false);
  }
  if (wos::native_renderer::Enabled())
    wos::capture::NoteMeshDraw();
  __imp__sub_82914058(ctx, base);
}

extern "C" void __imp__sub_82ABA950(PPCContext &ctx, uint8_t *base);
extern "C" void sub_82ABA950(PPCContext &ctx, uint8_t *base) {
  if (!wos::native_renderer::Enabled()) {
    __imp__sub_82ABA950(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  wos::native_renderer::BeforeKick(device);
  __imp__sub_82ABA950(ctx, base);
  wos::native_renderer::AfterKick(device);
}

// fix48: D3D's segment switch (sub_82ABA6E0: closes the current command-buffer segment and
// moves the write cursor to the next one; device+14928 = the new segment's start). The kick
// above calls it, but so does D3D's ring reserve (sub_82ABACF0) when a segment fills up -
// with no kick. The mirror then parsed from the old segment's end across the gap into the
// new segment: stale ring memory from an earlier lap (old vertex data: floats, identity
// matrices, ffffffff), so the span was rejected whole (~5 a frame, 20-240 KB) and the new
// segment's first packets - the next draws' fetch constants and constants - were lost:
// one-frame wrong textures/meshes. Now the old segment is read up to the cursor first and
// reading resumes at the new segment's start, exactly as for the kick.
extern "C" void __imp__sub_82ABA6E0(PPCContext &ctx, uint8_t *base);
extern "C" void sub_82ABA6E0(PPCContext &ctx, uint8_t *base) {
  if (!wos::native_renderer::Enabled()) {
    __imp__sub_82ABA6E0(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  wos::native_renderer::BeforeKick(device);
  __imp__sub_82ABA6E0(ctx, base);
  wos::native_renderer::AfterSegmentSwitch(device);
}

// fix49: the render list push (sub_82920340: links render item r3 into the current view's
// list at view+1052 - every world mesh, the buildings' depth-prepass stand-ins included, gets
// there through it; sub_82920AF0 is its only caller, reached through vtables). On a capture
// frame each push's guest call stack is recorded (blob 12, key = push serial; the item address
// is the first word, then the stack) - the building code that pushes the stand-ins is where
// the medium LOD is chosen (or skipped).
extern "C" void __imp__sub_82920340(PPCContext &ctx, uint8_t *base);
extern "C" void sub_82920340(PPCContext &ctx, uint8_t *base) {
  // fix92: on both renderers; blob 15 (same key): the item, the current view (0x82F6AF98),
  // item + 68 (the object its draw gets) and the item's first 128 bytes, after the push
  const bool cap = wos::capture::Active();
  const uint32_t item = ctx.r3.u32;
  static std::atomic<uint32_t> serial{0};
  const uint32_t key = cap ? serial.fetch_add(1, std::memory_order_relaxed) : 0;
  if (cap) {
    uint8_t blob[17 * 4];
    PutBe32(blob, item);
    const uint32_t n = wos::native_renderer::GuestCallStack(blob + 4);
    wos::capture::NoteBlob(12, key, blob, 4 * (1 + n));
  }
  __imp__sub_82920340(ctx, base);
  if (cap && eot::mem::readable(item, 128)) {
    uint8_t blob[12 + 128];
    PutBe32(blob, item);
    PutBe32(blob + 4, eot::mem::load<uint32_t>(0x82F6AF98u));
    PutBe32(blob + 8, eot::mem::load<uint32_t>(item + 68));
    std::memcpy(blob + 12, eot::mem::at<const uint8_t>(item), 128);
    wos::capture::NoteBlob(15, key, blob, sizeof blob);
  }
}

// fix92: the render item each D3D draw belongs to. sub_8291F508(r3 = item + 68's object) draws
// one render item; the D3D draws inside it record (blob 14, key = draw serial) that object and
// the current view, on both renderers - to link a draw (by its vertex buffer) to its push.
namespace {
thread_local uint32_t g_draw_item_obj = 0;
}
namespace wos::native_renderer {
// fix98: emulated only - the VS/PS constants the GPU sees for this draw: the stream parsed
// AFTER D3D's draw (its state flush writes the pending shadow), blob 20 = c0-c255 VS then PS.
void NoteDrawConstantsAfter(uint32_t key, uint32_t device) {
  if (Enabled() || !device)
    return;
  {
    std::lock_guard lock(g_stream_lock);
    g_cap_alu_on.store(true, std::memory_order_relaxed);
    StreamState &st = StreamFor(device);
    uint64_t mask[2] = {0, 0};
    SyncStream(st, device, false, mask);
  }
  std::vector<uint8_t> cb(0x800 * 4);
  for (uint32_t i = 0; i < 0x800; ++i)
    PutBe32(cb.data() + 4 * i, g_cap_alu[i]);
  wos::capture::NoteBlob(20, key, cb.data(), static_cast<uint32_t>(cb.size()));
}
void NoteDrawItem(uint32_t key, uint32_t device) {
  // fix94: blob 17 = the VS constants c0..c255 from the stream (both renderers). Emulated:
  // the stream is parsed here, read-only (nothing applied to the device), on capture frames.
  {
    if (!Enabled() && device) {
      g_cap_alu_on.store(true, std::memory_order_relaxed);
      std::lock_guard lock(g_stream_lock);
      StreamState &st = StreamFor(device);
      uint64_t mask[2] = {0, 0};
      SyncStream(st, device, false, mask);
    }
    if (Enabled())  // fix97: blob 18 = the flush before this draw (pending, before, after)
      wos::capture::NoteBlob(18, key, g_flush_before, sizeof g_flush_before);
    uint8_t cb[0x400 * 4];
    for (uint32_t i = 0; i < 0x400; ++i)
      PutBe32(cb + 4 * i, g_cap_alu[i]);
    wos::capture::NoteBlob(17, key, cb, sizeof cb);
  }
  uint8_t blob[8];
  PutBe32(blob, g_draw_item_obj);
  PutBe32(blob + 4, eot::mem::load<uint32_t>(0x82F6AF98u));
  wos::capture::NoteBlob(14, key, blob, sizeof blob);
}
}  // namespace wos::native_renderer
// fix93: a view's render list as it is drawn (sub_8291FA68, r3 = first element, +4 next,
// +28 flag, +68 the element's object): blob 16, key = call serial - the current view, then per
// element its address and first 96 bytes. On a capture frame, both renderers.
extern "C" void __imp__sub_8291FA68(PPCContext &ctx, uint8_t *base);
extern "C" void sub_8291FA68(PPCContext &ctx, uint8_t *base) {
  if (wos::capture::Active()) {
    static std::atomic<uint32_t> serial{0};
    std::vector<uint8_t> blob(8);
    PutBe32(blob.data(), eot::mem::load<uint32_t>(0x82F6AF98u));
    uint32_t e = ctx.r3.u32, n = 0;
    while (e && n < 1024 && eot::mem::readable(e, 96)) {
      const size_t o = blob.size();
      blob.resize(o + 100);
      PutBe32(blob.data() + o, e);
      std::memcpy(blob.data() + o + 4, eot::mem::at<const uint8_t>(e), 96);
      e = eot::mem::load<uint32_t>(e + 4);
      ++n;
    }
    PutBe32(blob.data() + 4, n);
    wos::capture::NoteBlob(16, serial.fetch_add(1, std::memory_order_relaxed), blob.data(),
                           static_cast<uint32_t>(blob.size()));
  }
  __imp__sub_8291FA68(ctx, base);
}

extern "C" void __imp__sub_8291F508(PPCContext &ctx, uint8_t *base);
extern "C" void sub_8291F508(PPCContext &ctx, uint8_t *base) {
  const uint32_t saved = g_draw_item_obj;
  g_draw_item_obj = ctx.r3.u32;
  __imp__sub_8291F508(ctx, base);
  g_draw_item_obj = saved;
}

// fix97: the game's SetVertexShaderConstantF / SetPixelShaderConstantF in the capture's event
// order (regs: device, start register, data pointer, count), both renderers
extern "C" void __imp__sub_82AB3FE8(PPCContext &ctx, uint8_t *base);
extern "C" void sub_82AB3FE8(PPCContext &ctx, uint8_t *base) {
  if (wos::capture::Active()) {
    wos::capture::Call("SetVSConstF", ctx, base, false);
    // blob 19 (serial): start register, count, then the data the game passes (up to 64 regs)
    static std::atomic<uint32_t> serial{0};
    const uint32_t start = ctx.r4.u32, data = ctx.r5.u32, count = std::min(ctx.r6.u32, 64u);
    std::vector<uint8_t> b(8 + 16 * count);
    PutBe32(b.data(), start);
    PutBe32(b.data() + 4, ctx.r6.u32);
    if (count && eot::mem::readable(data, 16 * count))
      std::memcpy(b.data() + 8, eot::mem::at<const uint8_t>(data), 16 * count);
    wos::capture::NoteBlob(19, serial.fetch_add(1, std::memory_order_relaxed), b.data(),
                           static_cast<uint32_t>(b.size()));
  }
  __imp__sub_82AB3FE8(ctx, base);
}
extern "C" void __imp__sub_82AB40C8(PPCContext &ctx, uint8_t *base);
extern "C" void sub_82AB40C8(PPCContext &ctx, uint8_t *base) {
  if (wos::capture::Active())
    wos::capture::Call("SetPSConstF", ctx, base, false);
  __imp__sub_82AB40C8(ctx, base);
}

extern "C" void __imp__sub_82AC12D8(PPCContext &ctx, uint8_t *base);
extern "C" void sub_82AC12D8(PPCContext &ctx, uint8_t *base) {
  const uint32_t type = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  if (wos::native_renderer::Enabled())
    wos::capture::Call("RingConstants", ctx, base, false);
  __imp__sub_82AC12D8(ctx, base);
  static std::atomic<uint32_t> logged[2]{};
  if (wos::native_renderer::Enabled() && logged[type & 1].load(std::memory_order_relaxed) < 3 &&
      logged[type & 1].fetch_add(1) < 3)
    REXLOG_INFO("[WoS-RENDERER] ring constants: {} c{}..c{}", type ? "pixel" : "vertex", start,
                start + count - 1);
}

extern "C" void __imp__sub_82ABE800(PPCContext &ctx, uint8_t *base);
extern "C" void wos_eot_BeginVertices(PPCContext &ctx, uint8_t *base);
extern "C" void sub_82ABE800(PPCContext &ctx, uint8_t *base) {
  if (wos::native_renderer::Enabled()) {
    const uint32_t device = ctx.r3.u32;
    wos::native_renderer::FlushRingConstants(device);
    wos::capture::Call("BeginVertices", ctx, base, true);
    wos_eot_BeginVertices(ctx, base);
    wos::native_renderer::SkipStreamCursor(device);
  } else {
    __imp__sub_82ABE800(ctx, base);
  }
}

// D3DDevice_Resolve, with EndTiling's per-tile resolves (sub_82AC9170: one per tile, its
// rect in screen space, a resolve-and-clear on all but the last) made one: the first copies
// the whole surface - drawn whole here - with no clear (BeginTiling clears the next frame),
// the rest run only D3D's own resolve.
extern "C" void __imp__sub_82ABFCB0(PPCContext &ctx, uint8_t *base);
extern "C" void wos_eot_Resolve(PPCContext &ctx, uint8_t *base);
namespace {
std::atomic<uint32_t> g_end_tiling_resolves{0};
}
extern "C" void sub_82ABFCB0(PPCContext &ctx, uint8_t *base) {
  if (!wos::native_renderer::Enabled()) {
    __imp__sub_82ABFCB0(ctx, base);
    return;
  }
  wos::capture::Call("wos_eot_Resolve", ctx, base, false);
  if (eot::gpu::wos_tiling::InEndTiling()) {
    if (g_end_tiling_resolves.fetch_add(1, std::memory_order_relaxed) == 0) {
      ctx.r4.u64 = ctx.r4.u32 & ~0x300u;  // no clear after it
      ctx.r5.u64 = 0;                       // the whole source
      ctx.r7.u64 = 0;                       // to the destination's origin
      wos_eot_Resolve(ctx, base);
    } else {
      __imp__sub_82ABFCB0(ctx, base);
    }
    return;
  }
  wos_eot_Resolve(ctx, base);
}

// D3DDevice_EndTiling (device, resolve flags, rects, dest texture, ...)
extern "C" void __imp__sub_82AC9170(PPCContext &ctx, uint8_t *base);
extern "C" void sub_82AC9170(PPCContext &ctx, uint8_t *base) {
  if (!wos::native_renderer::Enabled()) {
    __imp__sub_82AC9170(ctx, base);
    return;
  }
  wos::capture::Call("EndTiling", ctx, base, false);
  g_end_tiling_resolves.store(0, std::memory_order_relaxed);
  eot::gpu::wos_tiling::SetEndTiling(true);
  __imp__sub_82AC9170(ctx, base);
  eot::gpu::wos_tiling::SetEndTiling(false);
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 2)
    REXLOG_INFO("[WoS-RENDERER] EndTiling: {} tile resolves -> 1 whole",
                g_end_tiling_resolves.load());
}
