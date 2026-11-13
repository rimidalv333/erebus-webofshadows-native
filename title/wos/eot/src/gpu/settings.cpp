// gpu/settings.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/settings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>

#include <rex/cvar.h>

#include "core/logging.h"

REXCVAR_DEFINE_INT32(eot_trace_frames, 0, "EdgeOfTime/Debug", "Frames of D3D call tracing")
    .range(0, 100000);

REXCVAR_DEFINE_INT32(eot_trace_start_frame, 0, "EdgeOfTime/Debug", "Frame the trace starts")
    .range(0, 100000000);

REXCVAR_DEFINE_INT32(eot_summary_frames, 0, "EdgeOfTime/Debug", "Frames of call summaries")
    .range(0, 1000000);

REXCVAR_DEFINE_INT32(eot_diag, 1, "EdgeOfTime/Debug", "Renderer log verbosity")
    .range(0, 2);

REXCVAR_DEFINE_BOOL(eot_vsync, true, "EdgeOfTime/Video", "Sync frames to display");
REXCVAR_DEFINE_BOOL(eot_profiler, false, "EdgeOfTime/Debug", "Start Tracy at boot");
REXCVAR_DEFINE_INT32(eot_hitch_ms, 40, "EdgeOfTime/Debug", "Log frames slower than this")
    .range(0, 1000);
REXCVAR_DEFINE_DOUBLE(eot_render_scale, 1.0, "EdgeOfTime/Video", "Internal render scale")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(eot_resolution, "1080p", "EdgeOfTime/Video", "Internal render resolution")
    .allowed({"native", "720p", "1080p", "1440p", "2160p", "display"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(eot_fullscreen_mode, "borderless", "EdgeOfTime/Video", "Borderless or exclusive fullscreen")
    .allowed({"borderless", "exclusive"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(eot_pip_scale, 50, "EdgeOfTime/Video", "Picture-in-picture resolution")
    .range(25, 100)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(eot_aspect_ratio, "16:9", "EdgeOfTime/Video", "Fullscreen aspect ratio")
    .allowed({"auto", "4:3", "16:9", "16:10", "21:9", "32:9"});
REXCVAR_DEFINE_INT32(eot_fps_limit, 60, "EdgeOfTime/Video", "Frame rate cap")
    .range(0, 1000);
REXCVAR_DEFINE_BOOL(eot_fast_setters_verify, false, "EdgeOfTime/Debug", "Check fast setters match");
REXCVAR_DEFINE_BOOL(wos_fast_setters, true, "WoS",
                    "fix105: native SetTexture/SetVertexShader/SetPixelShader/SetStreamSource/"
                    "SetIndices (EoT's, with Web of Shadows' offsets) instead of the game's own");
REXCVAR_DEFINE_STRING(eot_host_aa, "off", "EdgeOfTime/Graphics", "Our anti-aliasing, off or TAA")
    .allowed({"off", "taa"});
REXCVAR_DEFINE_DOUBLE(eot_taa_feedback, 0.8, "EdgeOfTime/Graphics", "TAA history strength")
    .range(0.3, 0.95);
REXCVAR_DEFINE_BOOL(eot_motion_vectors, false, "EdgeOfTime/Graphics", "Per-object motion vectors");
REXCVAR_DEFINE_BOOL(eot_bloom, true, "EdgeOfTime/Graphics", "Glow around bright lights");
REXCVAR_DEFINE_BOOL(eot_depth_of_field, true, "EdgeOfTime/Graphics", "Depth of field blur");
REXCVAR_DEFINE_BOOL(eot_scene_copy_refresh, true, "EdgeOfTime/Graphics", "Refresh copies for glass effects");
REXCVAR_DEFINE_BOOL(eot_motion_blur, true, "EdgeOfTime/Graphics", "Motion blur on or off");
REXCVAR_DEFINE_BOOL(eot_color_grading, true, "EdgeOfTime/Graphics", "Scene color grading");
REXCVAR_DEFINE_DOUBLE(eot_fov_scale, 1.0, "EdgeOfTime/Graphics", "Field of view scale")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(eot_shadow_cascades, 0, "EdgeOfTime/Graphics", "Shadow cascades per camera");
REXCVAR_DEFINE_DOUBLE(eot_shadow_distance_scale, 1.0, "EdgeOfTime/Graphics", "Shadow distance multiplier");
REXCVAR_DEFINE_INT32(eot_debug_quality_level, -1, "EdgeOfTime/Debug", "Force the pak language id");
REXCVAR_DEFINE_INT32(eot_shadow_map_size, 0, "EdgeOfTime/Graphics", "Shadow map resolution")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(wos_anisotropy, 16, "WoS",
                     "Anisotropic filtering: 0 = the level the game asks for, 1 = off, 2-16 = at "
                     "least this level")
    .range(0, 16);
REXCVAR_DEFINE_INT32(wos_msaa, 4, "WoS", "Multisample anti-aliasing: 0 (off), 2, 4 or 8 samples")
    .range(0, 8)
    .validator([](std::string_view v) {
      int n = 0;
      const auto r = std::from_chars(v.data(), v.data() + v.size(), n);
      return r.ec == std::errc() && (n == 0 || n == 2 || n == 4 || n == 8);
    })
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(eot_brightness, 0.0, "EdgeOfTime/Video", "Screen brightness offset");
REXCVAR_DEFINE_DOUBLE(eot_contrast, 1.0, "EdgeOfTime/Video", "Screen contrast amount");
REXCVAR_DEFINE_DOUBLE(eot_saturation, 1.0, "EdgeOfTime/Video", "Screen color saturation");
REXCVAR_DEFINE_DOUBLE(eot_gamma, 1.0, "EdgeOfTime/Video", "Screen gamma curve");
REXCVAR_DEFINE_INT32(eot_perf_frames, 600, "EdgeOfTime/Debug", "Perf log every N frames");

REXCVAR_DEFINE_INT32(eot_vram_report_seconds, 0, "EdgeOfTime/Debug", "Seconds between VRAM reports")
    .range(0, 3600);
REXCVAR_DEFINE_BOOL(eot_vram_csv, false, "EdgeOfTime/Debug", "Write VRAM allocation CSVs");

REXCVAR_DEFINE_INT32(eot_diag_frame, 0, "EdgeOfTime/Debug", "Frame to log in detail")
    .range(0, 100000000);
REXCVAR_DEFINE_INT32(eot_diag_scene_from, 0, "EdgeOfTime/Debug", "First frame for scene diag")
    .range(0, 100000000);
REXCVAR_DEFINE_BOOL(eot_diag_scene, false, "EdgeOfTime/Debug", "Detail log the first scene");
REXCVAR_DEFINE_INT32(eot_diag_hitch, 0, "EdgeOfTime/Debug", "Detail log slow frames");
REXCVAR_DEFINE_BOOL(eot_record, false, "EdgeOfTime/Debug", "Record GPU frames periodically");
REXCVAR_DEFINE_BOOL(eot_diag_hitch_any, false, "EdgeOfTime/Debug", "Hitch diag on any frame");
REXCVAR_DEFINE_BOOL(eot_d3d12_debug, false, "EdgeOfTime/Debug", "D3D12 debug layer");
REXCVAR_DEFINE_BOOL(eot_diag_dump, false, "EdgeOfTime/Debug", "Dump resolve images");

REXCVAR_DEFINE_INT32(eot_rdc_frame, 0, "EdgeOfTime/Debug", "Frame to capture in RenderDoc")
    .range(0, 100000000);

REXCVAR_DEFINE_STRING(eot_rdc_dll,
                      "C:/Users/rieng/Documents/GitHub/renderdoc/x64/Development/renderdoc.dll",
                      "EdgeOfTime/Debug", "Path to renderdoc.dll");

REXCVAR_DEFINE_STRING(eot_rdc_path, "captures/reeot", "EdgeOfTime/Debug", "RenderDoc capture path");

REXCVAR_DEFINE_INT32(eot_dump_every, 0, "EdgeOfTime/Debug", "Save a frame every N")
    .range(0, 1000000);
REXCVAR_DEFINE_INT32(eot_dump_burst, 1, "EdgeOfTime/Debug", "Frames saved per dump")
    .range(1, 64);

REXCVAR_DEFINE_BOOL(eot_present_gamma, true, "EdgeOfTime/Video", "Use the console gamma ramp");

REXCVAR_DEFINE_BOOL(eot_present_log, false, "EdgeOfTime/Debug", "Log each present time");

namespace eot::gpu {

i32 Settings::TraceFrames() { return REXCVAR_GET(eot_trace_frames); }
i32 Settings::SummaryFrames() { return REXCVAR_GET(eot_summary_frames); }
i32 Settings::TraceStartFrame() { return REXCVAR_GET(eot_trace_start_frame); }
i32 Settings::DiagVerbosity() { return REXCVAR_GET(eot_diag); }
bool Settings::Vsync() { return REXCVAR_GET(eot_vsync); }
bool Settings::FastSettersVerify() { return REXCVAR_GET(eot_fast_setters_verify); }
bool Settings::FastSetters() { return REXCVAR_GET(wos_fast_setters); }
f64 Settings::RenderScale() { return REXCVAR_GET(eot_render_scale); }
std::string Settings::Resolution() { return std::string(REXCVAR_GET(eot_resolution)); }
std::string Settings::FullscreenMode() { return std::string(REXCVAR_GET(eot_fullscreen_mode)); }
i32 Settings::PipScalePercent() { return REXCVAR_GET(eot_pip_scale); }
i32 Settings::FpsLimit() { return REXCVAR_GET(eot_fps_limit); }
std::string Settings::AspectRatio() { return std::string(REXCVAR_GET(eot_aspect_ratio)); }
i32 Settings::HitchMs() { return REXCVAR_GET(eot_hitch_ms); }
bool Settings::Profiler() { return REXCVAR_GET(eot_profiler); }
i32 Settings::PerfFrames() { return REXCVAR_GET(eot_perf_frames); }
i32 Settings::VramReportSeconds() { return REXCVAR_GET(eot_vram_report_seconds); }
bool Settings::VramCsv() { return REXCVAR_GET(eot_vram_csv); }
i32 Settings::DumpEvery() { return REXCVAR_GET(eot_dump_every); }
i32 Settings::DumpBurst() { return REXCVAR_GET(eot_dump_burst); }
namespace {
std::atomic<i32> g_diag_frame_armed{0};
}
i32 Settings::DiagFrame() {
  const i32 armed = g_diag_frame_armed.load(std::memory_order_relaxed);
  return armed > 0 ? armed : REXCVAR_GET(eot_diag_frame);
}
bool Settings::DiagScene() { return REXCVAR_GET(eot_diag_scene); }
i32 Settings::DiagSceneFrom() { return REXCVAR_GET(eot_diag_scene_from); }
bool Settings::DiagDump() { return REXCVAR_GET(eot_diag_dump); }
i32 Settings::DiagHitch() { return REXCVAR_GET(eot_diag_hitch); }
bool Settings::DiagHitchAny() { return REXCVAR_GET(eot_diag_hitch_any); }
bool Settings::Record() { return REXCVAR_GET(eot_record); }
bool Settings::D3D12Debug() { return REXCVAR_GET(eot_d3d12_debug); }
void Settings::ArmDiagFrame(i32 frame) { g_diag_frame_armed.store(frame, std::memory_order_relaxed); }
namespace {
std::atomic<i32> g_dump_frame{-1};  // -1: none armed by L3+R3 yet
}
// WoS port (fix103): the automatic dump of guest frame 3000 (a ~2 s freeze) is off unless
// eot_auto_dump_frame names a frame; L3+R3 still arms its own
REXCVAR_DEFINE_INT32(eot_auto_dump_frame, 0, "EdgeOfTime/Debug",
                     "Guest frame dumped automatically with eot_diag_dump (0 = only L3+R3)");
i32 Settings::DumpFrame() {
  const i32 armed = g_dump_frame.load(std::memory_order_relaxed);
  if (armed >= 0)
    return armed;
  const i32 automatic = REXCVAR_GET(eot_auto_dump_frame);
  return automatic > 0 ? automatic : INT32_MAX;
}
void Settings::ArmDumpFrame(i32 frame) { g_dump_frame.store(frame, std::memory_order_relaxed); }
i32 Settings::RenderDocFrame() { return REXCVAR_GET(eot_rdc_frame); }
std::string Settings::RenderDocDll() { return std::string(REXCVAR_GET(eot_rdc_dll)); }
std::string Settings::RenderDocPath() { return std::string(REXCVAR_GET(eot_rdc_path)); }
bool Settings::PresentGamma() { return REXCVAR_GET(eot_present_gamma); }
bool Settings::Taa() { return kHostAaAvailable && REXCVAR_GET(eot_host_aa) == "taa"; }
bool Settings::MotionVectors() { return REXCVAR_GET(eot_motion_vectors); }
double Settings::TaaFeedback() { return REXCVAR_GET(eot_taa_feedback); }
bool Settings::Bloom() { return REXCVAR_GET(eot_bloom); }
bool Settings::DepthOfField() { return REXCVAR_GET(eot_depth_of_field); }
bool Settings::MotionBlur() { return REXCVAR_GET(eot_motion_blur); }
bool Settings::ColorGrading() { return REXCVAR_GET(eot_color_grading); }
bool Settings::SceneCopyRefresh() { return REXCVAR_GET(eot_scene_copy_refresh); }
double Settings::FovScale() { return REXCVAR_GET(eot_fov_scale); }
i32 Settings::Anisotropy() { return REXCVAR_GET(wos_anisotropy); }
i32 Settings::Msaa() { return REXCVAR_GET(wos_msaa); }
i32 Settings::QualityLevel() { return REXCVAR_GET(eot_debug_quality_level); }
i32 Settings::ShadowCascades() { return REXCVAR_GET(eot_shadow_cascades); }
i32 Settings::ShadowMapSize() { return REXCVAR_GET(eot_shadow_map_size); }
double Settings::ShadowDistanceScale() { return REXCVAR_GET(eot_shadow_distance_scale); }
double Settings::Brightness() { return REXCVAR_GET(eot_brightness); }
double Settings::Contrast() { return REXCVAR_GET(eot_contrast); }
double Settings::Saturation() { return REXCVAR_GET(eot_saturation); }
double Settings::Gamma() { return REXCVAR_GET(eot_gamma); }
bool Settings::PresentFrameLog() { return REXCVAR_GET(eot_present_log); }

namespace {

u32 g_auto_render_height = 1080;
u32 g_display_height = 0;

std::atomic<bool> g_fullscreen{true};
struct FullscreenWatch {
  FullscreenWatch() {
    rex::cvar::RegisterChangeCallback("fullscreen", [](std::string_view, std::string_view value) {
      g_fullscreen.store(value == "true" || value == "1", std::memory_order_relaxed);
    });
  }
} g_fullscreen_watch;

u32 PresetRows(std::string preset) {
  for (char &c : preset)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (preset == "4k" || preset == "uhd")
    return 2160;
  if (preset == "2k" || preset == "qhd")
    return 1440;
  if (preset == "fhd")
    return 1080;
  if (preset == "hd")
    return 720;
  if (!preset.empty() && preset.back() == 'p')
    preset.pop_back();
  if (preset.empty() || preset.find_first_not_of("0123456789") != std::string::npos)
    return 0;
  const u32 rows = static_cast<u32>(std::strtoul(preset.c_str(), nullptr, 10));
  return rows >= 360 && rows <= 4320 ? rows : 0;
}

f32 ComputeRenderScale() {
  const std::string preset = Settings::Resolution();
  u32 target_height = 0;
  if (!Settings::Fullscreen())
    target_height = g_auto_render_height;
  else if (preset == "display")
    target_height = g_display_height ? g_display_height : g_auto_render_height;
  else if (preset != "native") {
    target_height = PresetRows(preset);
    if (target_height == 0) {
      target_height = g_auto_render_height;
      EOT_WARN("[gpu] eot_resolution \"{}\" is not a preset (native, 720p, 1080p, 1440p, 2160p, display); "
               "rendering at the {} rows the display suggests",
               preset, target_height);
    }
  }
  const f64 base = target_height != 0
                       ? static_cast<f64>(target_height) / static_cast<f64>(kGuestRenderHeight)
                       : 1.0;
  const f64 scale = base * Settings::RenderScale();
  return static_cast<f32>(std::clamp(scale, 0.25, 5.0));
}

}

void SetAutoRenderHeight(u32 height) { g_auto_render_height = height; }
void SetDisplayHeight(u32 height) { g_display_height = height; }

bool Settings::Fullscreen() {
  static const bool initial = [] {
    const bool on = rex::cvar::Query<bool>("fullscreen");
    g_fullscreen.store(on, std::memory_order_relaxed);
    return on;
  }();
  (void)initial;
  return g_fullscreen.load(std::memory_order_relaxed);
}

std::string Settings::EffectiveAspectRatio() { return Fullscreen() ? AspectRatio() : std::string("auto"); }

f32 RenderScaleFactor() {
  static const f32 s = ComputeRenderScale();
  return s;
}

u32 InternalRenderWidth() { return ScaleDim(kGuestRenderWidth); }
u32 InternalRenderHeight() { return ScaleDim(kGuestRenderHeight); }

}

REXCVAR_DEFINE_STRING(eot_quality_preset, "custom", "EdgeOfTime/Graphics", "Graphics quality preset")
    .allowed({"low", "medium", "high", "ultra", "custom"});

namespace {

enum class Level { kLow, kMedium, kHigh, kUltra, kCustom };

struct Gate {
  const char *name;
  const char *low, *medium, *high, *ultra;
};

constexpr Gate kGates[] = {
    {"wos_msaa", "0", "2", "4", "8"},
    {"wos_upscale", "bilinear", "bicubic", "bicubic", "lanczos"},
    {"wos_anisotropy", "0", "8", "16", "16"},
    {"eot_shadow_map_size", "1024", "2048", "4096", "8192"},
    {"eot_host_aa", "off", "off", "off", eot::gpu::kHostAaAvailable ? "taa" : "off"},
    {"eot_motion_vectors", "false", "false", "false", "true"},
};

Level Parse(std::string_view v) {
  if (v == "low")
    return Level::kLow;
  if (v == "medium")
    return Level::kMedium;
  if (v == "high")
    return Level::kHigh;
  if (v == "ultra")
    return Level::kUltra;
  return Level::kCustom;
}

const char *Value(const Gate &g, Level level) {
  switch (level) {
  case Level::kLow:
    return g.low;
  case Level::kMedium:
    return g.medium;
  case Level::kHigh:
    return g.high;
  case Level::kUltra:
    return g.ultra;
  default:
    return nullptr;
  }
}

bool Same(std::string_view a, std::string_view b) {
  const std::string sa(a), sb(b);
  char *ea = nullptr, *eb = nullptr;
  const double da = std::strtod(sa.c_str(), &ea);
  const double db = std::strtod(sb.c_str(), &eb);
  const bool na = ea != sa.c_str() && *ea == '\0';
  const bool nb = eb != sb.c_str() && *eb == '\0';
  if (na && nb)
    return da == db;
  return sa == sb;
}

std::atomic<bool> g_applying{false};
std::atomic<bool> g_booted{false};

void Apply(Level level, std::string_view name) {
  g_applying = true;
  std::string line;
  for (const Gate &g : kGates) {
    const char *value = Value(g, level);
    const std::string current = rex::cvar::GetFlagByName(g.name);
    if (!Same(current, value) && !rex::cvar::SetFlagByName(g.name, value))
      EOT_WARN("[quality] preset {}: {} rejected {}", name, g.name, value);
    line += std::string(line.empty() ? "" : ", ") + g.name + " " + value;
  }
  g_applying = false;
  EOT_INFO("[quality] preset {}: {}", name, line);
}

struct Registrar {
  Registrar() {
    rex::cvar::RegisterChangeCallback("eot_quality_preset", [](std::string_view, std::string_view value) {
      if (g_applying)
        return;
      const Level level = Parse(value);
      if (level != Level::kCustom)
        Apply(level, value);
    });
    for (const Gate &g : kGates) {
      rex::cvar::RegisterChangeCallback(g.name, [gate = &g](std::string_view, std::string_view value) {
        EOT_DEBUG("[quality] {} -> {} (applying {}, booted {}, preset {})", gate->name, value, g_applying.load(),
                  g_booted.load(), REXCVAR_GET(eot_quality_preset));
        if (g_applying || !g_booted)
          return;
        const Level level = Parse(REXCVAR_GET(eot_quality_preset));
        if (level == Level::kCustom || Same(value, Value(*gate, level)))
          return;
        EOT_INFO("[quality] {} set to {} by hand; the preset is now custom", gate->name, value);
        rex::cvar::SetFlagByName("eot_quality_preset", "custom");
      });
    }
  }
} g_registrar;

}

namespace eot::gpu {

void ApplyQualityPresetAtBoot() {
  static bool done = false;
  if (done)
    return;
  done = true;
  const std::string preset(REXCVAR_GET(eot_quality_preset));
  const Level level = Parse(preset);
  if (level != Level::kCustom)
    Apply(level, preset);
  g_booted = true;
}

}
