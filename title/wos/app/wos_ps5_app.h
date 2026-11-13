// WebOfShadowsRecomp for the PS5 - the game's ReXGlue app.
//
// The PS5 counterpart of WebOfShadowsRecomp's WosRecompApp (src/wos_recomp_app.h),
// written for the console rather than copied: the same guest-side hooks, none of
// the Windows host code.
//
//   kept      the GPU and engine settings that matter to the game (as cvars, in
//             wos/wos_recomp.toml beside eboot.bin, which the player can edit);
//             WebOfShadowsRecomp's unregistered guest function handler and its
//             swap hook (compiled from the builder's checkout); its guard on
//             invalid indirect call targets (wos/app/wos_ps5_hooks.cpp, through the
//             linker's --wrap instead of patching rexruntime.dll's import table);
//             its hash table lookup guard (sub_82A1ACF0, checking guest pages
//             instead of catching faults with SEH)
//   replaced  the D3D12 GPU plugin DLL: the emulated Xenos GPU linked into the title,
//             on Vulkan (RADV), created here; keyboard/mouse, XInput and SDL input:
//             the runtime's PS5 pad driver; the Windows crash sentinel and minidumps:
//             the runtime's PS5 fault records (logs/fault.log)
//   dropped   FSR/CAS settings, the "2x frame pacer", process priority and timer
//             tweaks, window-title telemetry, frame-capture debugging: all Windows or
//             D3D12 presenter features
//
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <string>

#include <rex/rex_app.h>

namespace wos {

class WosPs5App final : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

 protected:
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnLoadXexImage(std::string& xex_image) override;
  void OnPostLoadXexImage() override;
  void OnPostSetup() override;
  void OnPreLaunchModule() override;
  void OnPostLaunchModule(rex::system::XThread* thread) override;
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override;
  void OnShutdown() override;
};

}  // namespace wos
