// platform/display.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdint>

namespace eot::platform {

struct Display {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t refresh_hz = 0;
};

Display DisplayFor(void *native_window);

uint32_t AutoRenderHeight(const Display &display);
const char *AutoResolutionPreset(const Display &display);

const char *AutoAspectPreset(const Display &display);

uint32_t AutoFrameRateLimit(const Display &display);

}
