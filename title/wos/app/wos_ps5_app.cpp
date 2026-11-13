// WebOfShadowsRecomp for the PS5 - the game's ReXGlue app (wos/app/wos_ps5_app.h).
//
// SPDX-License-Identifier: MIT

#include "wos_ps5_app.h"

#include <rex/ui/immediate_drawer.h>
#include <rex/ui/window.h>

#include <cstdlib>
#include <mutex>
#include <array>
#include <cstdio>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <string_view>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/xthread.h>
#include <rex/ui/startup_prospero.h>

// The game's recompiled code: PPCImageConfig, REX_CODE_BASE and the rest
#include "generated/default/wos_recomp_init.h"
// WebOfShadowsRecomp's handler for guest functions the recompiler did not find
// (from the builder's checkout; its "hooks/perf_monitor.h" is wos/shim's)
#include "stubs/unregistered_func_handler.h"

// The host profiler (wos_ps5_threads.cpp): samples the emulated GPU's command
// processor 2500 times a second for /app0/wos-profile.txt. Off unless asked for:
// the signals cost the thread it measures a little of its time.
REXCVAR_DEFINE_BOOL(wos_profile, false, "WoS",
                    "Sample the GPU command processor for /app0/wos-profile.txt");

// WebOfShadowsRecomp (0028): the diagnostics master switch. Off by default, so a
// release build pays nothing for the per-draw timing probes, the command
// processor's idle/exec timing, or the per-frame perf log. On for a measurement
// run. wos_profile needs this on as well (it also starts the sampler).
REXCVAR_DEFINE_BOOL(wos_diagnostics, false, "WoS",
                    "Collect and log performance diagnostics (off for release)");
// Set in the emulated GPU before the game starts (src/graphics/command_processor.cpp)
extern "C" void rex_gpu_set_diagnostics(bool enabled);

// The emulated GPU, linked into the title (librexgpu-xenos.a, src/graphics/plugin_main.cpp)
extern "C" rex::system::IGraphicsSystem* rex_gpu_create(uint32_t abi_version,
                                                        const rex::system::GpuCreateInfo* info);

namespace wos {

namespace hooks {
// wos/app/wos_ps5_hooks.cpp
void SetRuntime(rex::Runtime* runtime);
}  // namespace hooks

namespace shader_dump {
// wos/app/wos_ps5_shader_dump.cpp - captures the game's shaders to
// /app0/wos-shaders.bin when wos_shader_dump is on (developer tool)
void Start(rex::Runtime* runtime);
}  // namespace shader_dump

namespace native {
// wos/app/wos_ps5_native.cpp - native renderer: intercepts the game's D3D draws
// when wos_native is on (the PPSA99311 "Native" title). Prototype: observe + log.
void Configure();
}  // namespace native

namespace {

// The settings the game needs or does best with, as WebOfShadowsRecomp's
// OnPreSetup sets them on the PC, minus the D3D12-only and Windows-only ones.
// Each applies only while the setting is at its built-in default: whatever
// wos_recomp.toml (beside eboot.bin) or the command line sets wins.
struct Setting {
  std::string_view name;
  std::string_view value;
};
constexpr std::array kGameSettings = {
    // The game reads fetch constants the emulator would otherwise reject
    Setting{"gpu_allow_invalid_fetch_constants", "true"},
    Setting{"clear_memory_page_state", "false"},
    // No CPU readback of resolves or memexport (both slow, and the game needs neither)
    Setting{"readback_resolve", "none"},
    Setting{"readback_memexport", "false"},
    Setting{"vulkan_readback_resolve", "false"},
    Setting{"vulkan_readback_memexport", "false"},
    // Occlusion queries: the engine culls the city's blocks behind the towers
    Setting{"occlusion_query_enable", "true"},
    Setting{"execute_unclipped_draw_vs_on_cpu", "false"},
    Setting{"execute_unclipped_draw_vs_on_cpu_with_scissor", "false"},
    Setting{"primitive_processor_cache_min_indices", "4096"},
    Setting{"native_2x_msaa", "false"},
    Setting{"use_fuzzy_alpha_epsilon", "true"},
    Setting{"async_shader_compilation", "true"},
    // The PS5's GPU memory is not a 4 GB laptop card's: the texture cache may keep more
    Setting{"texture_cache_memory_limit_soft", "1536"},
    Setting{"texture_cache_memory_limit_hard", "2560"},
    Setting{"texture_cache_memory_limit_render_to_texture", "128"},
    Setting{"texture_cache_memory_limit_soft_lifetime", "30"},
    // Fragment shader interlock (RADV has it), the Vulkan counterpart of the ROV
    // path WebOfShadowsRecomp's PC release went back to: with "fbo" the Bink
    // cutscenes and the title screen stayed black on the console. One allocation
    // for the GPU's copy of guest memory rather than sparse binding, which this
    // RADV has not been checked with.
    Setting{"render_target_path_vulkan", "fsi"},
    Setting{"vulkan_sparse_shared_memory", "false"},
    // The display: full screen, the game's 60 Hz
    Setting{"fullscreen", "true"},
    Setting{"vsync", "true"},
};

std::string Get(std::string_view name) {
  const auto* info = rex::cvar::GetFlagInfo(name);
  return info && info->getter ? info->getter() : std::string("(not in this runtime)");
}

void ApplyGameSettings() {
  for (const auto& setting : kGameSettings) {
    if (!rex::cvar::GetFlagInfo(setting.name)) {
      REXLOG_WARN("[WoS] setting {} is not in this runtime; skipped", setting.name);
      continue;
    }
    if (rex::cvar::GetFlagSource(setting.name) != rex::cvar::Source::kDefault) {
      continue;
    }
    if (!rex::cvar::SetFlagByName(setting.name, setting.value)) {
      REXLOG_WARN("[WoS] setting {} = {} was refused", setting.name, setting.value);
    }
  }
}

// Frames a second in the log (and klog through the startup log) every ten
// seconds, from the game's swap hook: what a report of how it runs needs
}  // namespace
extern "C" void rex_gpu_perf_take(uint64_t* out);
extern "C" void rex_gpu_perf_take2(uint64_t* out);
extern "C" void rex_gpu_perf_take3(uint64_t* out);
extern "C" void rex_mem_perf_take(uint64_t* out);
extern "C" void rex_sm_perf_take(uint64_t* out);
namespace threads {
void Report();  // wos_ps5_threads.cpp
void StartSampler();
}
namespace {

// What the console's basic operations cost, once at start: the command
// processor's draws are made of them (allocations, locks, clock reads)
void LogPrimitiveCosts() {
  using clock = std::chrono::steady_clock;
  constexpr int kCount = 200000;
  auto per_op_ns = [](clock::time_point start) {
    return std::chrono::duration<double, std::nano>(clock::now() - start).count() / kCount;
  };
  auto start = clock::now();
  for (int i = 0; i < kCount; ++i) {
    void* volatile p = std::malloc(64 + (i & 7) * 16);
    std::free(p);
  }
  const double malloc_ns = per_op_ns(start);
  std::recursive_mutex mutex;
  start = clock::now();
  for (int i = 0; i < kCount; ++i) {
    mutex.lock();
    mutex.unlock();
  }
  const double mutex_ns = per_op_ns(start);
  start = clock::now();
  uint64_t sink = 0;
  for (int i = 0; i < kCount; ++i) {
    sink += uint64_t(clock::now().time_since_epoch().count());
  }
  const double clock_ns = per_op_ns(start);
  // A 10 us sleep (the runtime's event pulse slept that long before patch 0021)
  start = clock::now();
  for (int i = 0; i < 200; ++i) {
    std::this_thread::sleep_for(std::chrono::microseconds(10));
  }
  const double sleep_us = std::chrono::duration<double, std::micro>(clock::now() - start).count() / 200;
  REXLOG_INFO("[WoS-PERF] costs: malloc+free {:.0f} ns, mutex lock+unlock {:.0f} ns, clock read {:.0f} ns, "
              "10 us sleep {:.0f} us ({})",
              malloc_ns, mutex_ns, clock_ns, sleep_us, sink & 1);
  char line[160];
  snprintf(line, sizeof line, "costs: malloc+free %.0f ns, mutex %.0f ns, clock %.0f ns, 10us sleep %.0f us",
           malloc_ns, mutex_ns, clock_ns, sleep_us);
  prospero_startup_log(line);
}

void StartFrameRateLog() {
  // 0028: nothing unless diagnostics (or profiling) were asked for - no per-frame
  // perf thread, no startup cost probe, no sampler.
  if (!REXCVAR_GET(wos_diagnostics) && !REXCVAR_GET(wos_profile)) {
    return;
  }
  LogPrimitiveCosts();
  if (REXCVAR_GET(wos_profile)) {
    threads::StartSampler();
  }
  std::thread([] {
    uint64_t last = wos::perf::g_total_frames.load(std::memory_order_relaxed);
    auto last_time = std::chrono::steady_clock::now();
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      const uint64_t now_frames = wos::perf::g_total_frames.load(std::memory_order_relaxed);
      const auto now = std::chrono::steady_clock::now();
      const double seconds = std::chrono::duration<double>(now - last_time).count();
      const double fps = seconds > 0 ? double(now_frames - last) / seconds : 0;
      // The GPU command processor's time over the same window (the runtime's
      // probe, wos/patches/rexglue/0002): waiting for the game's commands means
      // the game's own code is what holds the frame rate; blocked on fences, the
      // GPU; executing the rest, translating the commands
      uint64_t gpu[5] = {};
      rex_gpu_perf_take(gpu);
      const double window_ns = seconds * 1e9;
      const double idle = window_ns > 0 ? 100.0 * gpu[0] / window_ns : 0;
      const double exec = window_ns > 0 ? 100.0 * (gpu[1] - std::min(gpu[1], gpu[2])) / window_ns : 0;
      const double fence = window_ns > 0 ? 100.0 * gpu[2] / window_ns : 0;
      // ...and, within translating (0003), draws, resolves, swaps and submissions
      // (a submission inside a swap counts in both), with the draws a frame
      uint64_t parts[5] = {};
      rex_gpu_perf_take2(parts);
      auto share = [&](uint64_t ns) { return window_ns > 0 ? 100.0 * ns / window_ns : 0.0; };
      const double frames = double(now_frames - last);
      REXLOG_INFO("[WoS-PERF] translating: draws {:.0f}%, resolves {:.0f}%, swaps {:.0f}%, "
                  "submissions {:.0f}%; {:.0f} draws a frame",
                  share(parts[0]), share(parts[1]), share(parts[2]), share(parts[3]),
                  frames > 0 ? parts[4] / frames : 0.0);
      {
        char detail[160];
        snprintf(detail, sizeof detail, "draws %.0f%% resolves %.0f%% swaps %.0f%% submits %.0f%%; %.0f draws/frame",
                 share(parts[0]), share(parts[1]), share(parts[2]), share(parts[3]),
                 frames > 0 ? parts[4] / frames : 0.0);
        prospero_startup_log(detail);
      }
      // ...and within draws (0004): primitive processing, textures, render targets,
      // pipelines, bindings, vertex buffers; with the guest memory protection
      // changes and access faults behind the GPU's view of guest memory
      uint64_t steps[8] = {}, mem[4] = {};
      rex_gpu_perf_take3(steps);
      rex_mem_perf_take(mem);
      // ...and the shared memory requests behind primitives and vertex buffers (0005)
      uint64_t sm[4] = {};
      rex_sm_perf_take(sm);
      REXLOG_INFO("[WoS-PERF] shared memory: {} requests a frame, {:.0f}% of the time ({:.0f}% "
                  "waiting for the global lock, {:.0f}% uploading)",
                  frames > 0 ? sm[0] / frames : 0.0, share(sm[1]), share(sm[2]), share(sm[3]));
      {
        char detail[160];
        snprintf(detail, sizeof detail, "shared memory: %.0f requests/frame, %.0f%% (lock %.0f%%, upload %.0f%%)",
                 frames > 0 ? sm[0] / frames : 0.0, share(sm[1]), share(sm[2]), share(sm[3]));
        prospero_startup_log(detail);
      }
      REXLOG_INFO("[WoS-PERF] draws: primitives {:.0f}%, textures {:.0f}%, render targets {:.0f}%, "
                  "pipelines {:.0f}%, bindings {:.0f}%, vertex buffers {:.0f}%; memory: {} protects "
                  "({:.0f}%), {} faults ({:.0f}%)",
                  share(steps[0]), share(steps[1]), share(steps[2]), share(steps[3]), share(steps[4]),
                  share(steps[5]), mem[0], share(mem[1]), mem[2], share(mem[3]));
      {
        char detail[200];
        snprintf(detail, sizeof detail,
                 "in draws: prim %.0f%% tex %.0f%% rt %.0f%% pipe %.0f%% bind %.0f%% vtx %.0f%%; "
                 "%llu protects %.0f%%, %llu faults %.0f%%",
                 share(steps[0]), share(steps[1]), share(steps[2]), share(steps[3]), share(steps[4]),
                 share(steps[5]), (unsigned long long)mem[0], share(mem[1]),
                 (unsigned long long)mem[2], share(mem[3]));
        prospero_startup_log(detail);
      }
      {
        const uint64_t bindings = steps[6] + steps[7];
        char detail[120];
        snprintf(detail, sizeof detail, "texture bindings reused: %.0f%% (%.0f a frame written)",
                 bindings ? 100.0 * steps[6] / bindings : 0.0,
                 frames > 0 ? steps[7] / frames : 0.0);
        prospero_startup_log(detail);
      }
      threads::Report();
      REXLOG_INFO("[WoS-PERF] {:.1f} frames a second ({} frames so far); GPU commands: "
                  "{:.0f}% waiting for the game, {:.0f}% translating, {:.0f}% waiting for the GPU "
                  "({} waits, {} occlusion reads)",
                  fps, now_frames, idle, exec, fence, gpu[3], gpu[4]);
      char line[192];
      snprintf(line, sizeof line,
               "fps: %.1f (%llu frames); cp idle %.0f%% translate %.0f%% gpu-wait %.0f%% (%llu waits, %llu occl)",
               fps, (unsigned long long)now_frames, idle, exec, fence, (unsigned long long)gpu[3],
               (unsigned long long)gpu[4]);
      prospero_startup_log(line);
      last = now_frames;
      last_time = now;
    }
  }).detach();
}

}  // namespace

namespace headless {
bool Enabled();  // wos_ps5_headless.cpp
}
namespace native_renderer {
bool Enabled();  // wos_ps5_native_renderer.cpp
std::unique_ptr<rex::ui::ImmediateDrawer> CreateOverlayDrawer();
bool Start(rex::ui::Window* window);
void Shutdown();
}

std::unique_ptr<rex::ui::WindowedApp> WosPs5App::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<WosPs5App>(new WosPs5App(ctx, "wos_recomp", PPCImageConfig));
}

void WosPs5App::OnPreSetup(rex::RuntimeConfig& config) {
  ApplyGameSettings();

  // 0028: diagnostics on when asked for, or when profiling (which needs them).
  const bool diagnostics = REXCVAR_GET(wos_diagnostics) || REXCVAR_GET(wos_profile);
  rex_gpu_set_diagnostics(diagnostics);
  REXLOG_INFO("[WoS] diagnostics {}", diagnostics ? "on" : "off");

  // The emulated Xenos GPU on Vulkan. A title loads no shared library, so
  // ReXApp's plugin loader (rexgpu-<name>.so beside the executable) cannot find
  // it; the title links it and creates it here, which ReXApp then sets up
  // Native renderer P3 stage 1 (wos_ps5_headless.cpp): no emulated GPU at all, as Edge of
  // Time runs; ReXApp then runs detached (no presenter) and nothing is drawn
  if (headless::Enabled()) {
    config.graphics = nullptr;
    config.gpu_plugin.clear();
    if (native_renderer::Enabled()) {
      REXLOG_INFO("[WoS] OnPreSetup: NATIVE RENDERER - no emulated GPU, Edge of Time's renderer draws");
      prospero_startup_log("wos: NATIVE RENDERER - no emulated GPU (wos_native_renderer)");
    } else {
      REXLOG_INFO("[WoS] OnPreSetup: HEADLESS - no emulated GPU, nothing will be drawn");
      prospero_startup_log("wos: HEADLESS - no emulated GPU (wos_headless)");
    }
    return;
  }
  if (!config.graphics) {
    rex::system::GpuCreateInfo info{};
    info.struct_size = sizeof(info);
    info.backend = "vulkan";
    prospero_startup_log("wos: creating the emulated GPU (Vulkan)");
    config.graphics.reset(rex_gpu_create(rex::system::kGpuPluginAbiVersion, &info));
    if (!config.graphics) {
      REXLOG_ERROR("[WoS] the emulated GPU could not be created");
      prospero_startup_log("wos: the emulated GPU could NOT be created");
    }
  }
  config.gpu_plugin.clear();
  REXLOG_INFO("[WoS] OnPreSetup: GPU {}, render target path '{}', sparse shared memory {}",
              config.graphics ? "created" : "MISSING", Get("render_target_path_vulkan"),
              Get("vulkan_sparse_shared_memory"));
}

void WosPs5App::OnLoadXexImage(std::string& xex_image) {
  REXLOG_INFO("[WoS] OnLoadXexImage: loading '{}'", xex_image);
  prospero_startup_log("wos: loading default.xex");
}

void WosPs5App::OnPostLoadXexImage() {
  REXLOG_INFO("[WoS] OnPostLoadXexImage: the XEX is mapped");
  hooks::SetRuntime(runtime());
  prospero_startup_log("wos: installing the unregistered function handler");
  wos::stubs::InstallUnregisteredFuncFallbacks(runtime());
  prospero_startup_log("wos: hooks installed");
}

void WosPs5App::OnPostSetup() {
  REXLOG_INFO("[WoS] OnPostSetup: the runtime and its systems are ready");
  // Native renderer: read wos_native once and arm the draw hooks if on.
  native::Configure();
  for (std::string_view name : {"gpu_allow_invalid_fetch_constants", "render_target_path_vulkan",
                                "vulkan_sparse_shared_memory", "occlusion_query_enable",
                                "draw_resolution_scale_x", "draw_resolution_scale_y", "vsync",
                                "texture_cache_memory_limit_hard", "anisotropic_override",
                                "present_effect", "prospero_output_hz", "ignore_thread_affinities",
                                "primitive_processor_cache_min_indices", "wos_texture_dump",
                                "vulkan_async_submission", "vulkan_submit_on_primary_buffer_end",
                                "wos_profile", "wos_diagnostics", "wos_display_pacing",
                                "wos_adaptive_sync", "wos_even_pacing",
                                "wos_shader_dump", "wos_native", "video_mode_refresh_rate", "wos_headless", "wos_native_renderer", "wos_native_debug_log", "eot_diag_scene", "wos_capture_frame", "eot_diag_dump",
                                "wos_depth_shells", "wos_depth_float24", "wos_overlay_memo", "wos_facade_reflections",
                                "wos_debug_draw_stats", "wos_debug_null_slots", "wos_debug_cube_faces",
                                "wos_facade_max_distance", "wos_facade_occlusion", "wos_strip_fast_path", "wos_strip_merge", "wos_msaa", "wos_anisotropy", "wos_upscale", "wos_shader_harvest", "eot_perf_frames", "eot_hitch_ms"}) {
    if (const auto* info = rex::cvar::GetFlagInfo(name)) {
      REXLOG_INFO("[WoS-CVAR] {} = '{}' (default '{}')", name, Get(name), info->default_value);
    }
  }
}

void WosPs5App::OnPreLaunchModule() {
  if (native_renderer::Enabled() && !native_renderer::Start(window())) {
    REXLOG_ERROR("[WoS] the native renderer did not start; the game runs without a picture");
  }
  REXLOG_INFO("[WoS] OnPreLaunchModule: starting the main guest thread");
}

std::unique_ptr<rex::ui::ImmediateDrawer> WosPs5App::OnCreateImmediateDrawer() {
  return native_renderer::Enabled() ? native_renderer::CreateOverlayDrawer() : nullptr;
}

void WosPs5App::OnShutdown() {
  if (native_renderer::Enabled()) native_renderer::Shutdown();
}

void WosPs5App::OnPostLaunchModule(rex::system::XThread* thread) {
  REXLOG_INFO("[WoS] OnPostLaunchModule: main guest thread {:08X} (thread id {})",
              thread ? thread->handle() : 0, thread ? thread->thread_id() : 0);
  StartFrameRateLog();
  // Developer: capture the game's shaders for a native renderer experiment
  // (off unless wos_shader_dump is set in wos_recomp.toml)
  shader_dump::Start(runtime());
}

}  // namespace wos

REX_DEFINE_APP(wos_recomp, wos::WosPs5App::Create)
