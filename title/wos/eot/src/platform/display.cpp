// platform/display.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "platform/display.h"

#include <cmath>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(__PROSPERO__)
#include <SDL3/SDL.h>
#endif

namespace eot::platform {

namespace {

#if defined(_WIN32)
uint32_t RefreshRateOf(const wchar_t *gdi_device) {
  UINT32 paths = 0, modes = 0;
  if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) == ERROR_SUCCESS && paths) {
    std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes ? modes : 1);
    if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path.data(), &modes, mode.data(), nullptr) ==
        ERROR_SUCCESS) {
      for (UINT32 i = 0; i < paths; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path[i].sourceInfo.adapterId;
        source.header.id = path[i].sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            std::wcscmp(source.viewGdiDeviceName, gdi_device) != 0)
          continue;
        const DISPLAYCONFIG_RATIONAL &rate = path[i].targetInfo.refreshRate;
        if (rate.Denominator)
          return static_cast<uint32_t>(std::lround(static_cast<double>(rate.Numerator) / rate.Denominator));
      }
    }
  }
  DEVMODEW device_mode{};
  device_mode.dmSize = sizeof(device_mode);
  if (::EnumDisplaySettingsW(gdi_device, ENUM_CURRENT_SETTINGS, &device_mode) && device_mode.dmDisplayFrequency > 1)
    return device_mode.dmDisplayFrequency;
  return 0;
}
#endif

}

Display DisplayFor(void *native_window) {
  Display display;
#if defined(_WIN32)
  HMONITOR monitor = native_window ? ::MonitorFromWindow(static_cast<HWND>(native_window), MONITOR_DEFAULTTONEAREST)
                                   : ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (monitor && ::GetMonitorInfoW(monitor, &info)) {
    display.width = static_cast<uint32_t>(info.rcMonitor.right - info.rcMonitor.left);
    display.height = static_cast<uint32_t>(info.rcMonitor.bottom - info.rcMonitor.top);
    display.refresh_hz = RefreshRateOf(info.szDevice);
  }
#elif defined(__PROSPERO__)
  // The PS5's one display: the 3840x2160 mode nearest 60 Hz, which plume's
  // VK_KHR_display surface takes (ps5/patches/plume-ps5.patch)
  (void)native_window;
  display.width = 3840;
  display.height = 2160;
  display.refresh_hz = 60;
#else
  SDL_Window *sdl_window = static_cast<SDL_Window *>(native_window);
  if (!sdl_window) {
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    sdl_window = (windows && count > 0) ? windows[0] : nullptr;
    SDL_free(windows);
  }
  SDL_DisplayID id = sdl_window ? SDL_GetDisplayForWindow(sdl_window) : 0;
  if (!id)
    id = SDL_GetPrimaryDisplay();
  if (const SDL_DisplayMode *mode = id ? SDL_GetDesktopDisplayMode(id) : nullptr) {
    const float density = mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
    display.width = static_cast<uint32_t>(std::lround(mode->w * density));
    display.height = static_cast<uint32_t>(std::lround(mode->h * density));
    if (mode->refresh_rate > 1.0f)
      display.refresh_hz = static_cast<uint32_t>(std::lround(mode->refresh_rate));
  }
#endif
  return display;
}

uint32_t AutoRenderHeight(const Display &display) {
  const uint32_t height = display.height ? display.height : 1080;
  if (height >= 1440)
    return 1440;
  if (height >= 1080)
    return 1080;
  return 720;
}

const char *AutoResolutionPreset(const Display &display) {
  return AutoRenderHeight(display) >= 1080 ? "1080p" : "720p";
}

const char *AutoAspectPreset(const Display &display) {
  if (!display.width || !display.height)
    return "16:9";
  const double ratio = static_cast<double>(display.width) / static_cast<double>(display.height);
  struct Preset {
    const char *name;
    double ratio;
  };
  static constexpr Preset kPresets[] = {
      {"4:3", 4.0 / 3.0}, {"16:10", 16.0 / 10.0}, {"16:9", 16.0 / 9.0}, {"21:9", 21.0 / 9.0}, {"32:9", 32.0 / 9.0},
  };
  const Preset *best = &kPresets[2];
  for (const Preset &p : kPresets)
    if (std::fabs(p.ratio - ratio) < std::fabs(best->ratio - ratio))
      best = &p;
  return best->name;
}

uint32_t AutoFrameRateLimit(const Display &display) { return display.refresh_hz ? display.refresh_hz : 60; }

}
