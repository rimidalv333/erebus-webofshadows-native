// gpu/settings.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <algorithm>
#include <cmath>

#include <string>

#include <rex/types.h>

namespace eot::gpu {

// The temporal pass works on D3D12 only for now.
#if defined(EOT_D3D12)
inline constexpr bool kHostAaAvailable = true;
#else
inline constexpr bool kHostAaAvailable = false;
#endif

struct Settings {
  static i32 TraceFrames();
  static i32 TraceStartFrame();
  static i32 SummaryFrames();
  static i32 DiagVerbosity();
  static bool Vsync();
  static bool Taa();
  static double TaaFeedback();
  static bool MotionVectors();
  static bool Bloom();
  static bool DepthOfField();
  static bool MotionBlur();
  static bool ColorGrading();
  static bool SceneCopyRefresh();
  static double FovScale();
  static i32 Anisotropy();
  static i32 Msaa();
  static i32 QualityLevel();
  static i32 ShadowCascades();
  static i32 ShadowMapSize();
  static double ShadowDistanceScale();
  static double Brightness();
  static double Contrast();
  static double Saturation();
  static double Gamma();
  static bool FastSettersVerify();
  static bool FastSetters();
  static f64 RenderScale();
  static std::string Resolution();
  static std::string FullscreenMode();
  static i32 PipScalePercent();
  static i32 FpsLimit();
  static std::string AspectRatio();
  static std::string EffectiveAspectRatio();
  static bool Fullscreen();
  static i32 HitchMs();
  static bool Profiler();
  static i32 PerfFrames();
  static i32 VramReportSeconds();
  static bool VramCsv();
  static i32 DumpEvery();
  static i32 DumpBurst();
  static i32 DiagFrame();
  static bool DiagScene();
  static i32 DiagSceneFrom();
  static bool DiagDump();
  static i32 DiagHitch();
  static bool DiagHitchAny();
  static bool Record();
  static bool D3D12Debug();
  static void ArmDiagFrame(i32 frame);
  // WoS port (fix34): the guest frame eot_diag_dump dumps (3000, or re-armed by the pad
  // trigger L3+R3: wos/app/wos_ps5_capture.cpp)
  static i32 DumpFrame();
  static void ArmDumpFrame(i32 frame);
  static i32 RenderDocFrame();
  static std::string RenderDocDll();
  static std::string RenderDocPath();
  static bool PresentGamma();
  static bool PresentFrameLog();
};

// WoS port: Web of Shadows renders its scene at 1280x720 (Edge of Time: 1120x632).
constexpr u32 kGuestRenderWidth = 1280;
constexpr u32 kGuestRenderHeight = 720;

void SetAutoRenderHeight(u32 height);
void SetDisplayHeight(u32 height);
f32 RenderScaleFactor();
u32 InternalRenderWidth();
u32 InternalRenderHeight();
inline i32 ScalePx(i32 v) { return static_cast<i32>(std::lround(v * RenderScaleFactor())); }
inline i32 ScalePxBy(i32 v, float s) { return static_cast<i32>(std::lround(v * s)); }
inline u32 ScaleDimBy(u32 v, float s) {
  return std::max(1u, static_cast<u32>(std::lround(v * s)));
}
inline u32 ScaleDim(u32 v) {
  return std::max(1u, static_cast<u32>(std::lround(v * RenderScaleFactor())));
}
inline f32 ShadowMapTargetScale() {
  const i32 size = Settings::ShadowMapSize();
  const f32 want = size > 0 ? static_cast<f32>(size) / 1024.0f : RenderScaleFactor();
  return static_cast<f32>(std::clamp<i32>(static_cast<i32>(std::lround(want)), 1, 8));
}

}

namespace eot::gpu {

void ApplyQualityPresetAtBoot();

}
