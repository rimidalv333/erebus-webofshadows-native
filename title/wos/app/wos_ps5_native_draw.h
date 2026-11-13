// WebOfShadowsRecomp for the PS5 -- native renderer, Option A1 entry point.
// See wos_ps5_native_draw.cpp for the full description.
#pragma once

#include <cstdint>

namespace wos {
namespace native {

// One-shot A1 plumbing proof: draw `vertex_count` vertices read from guest memory
// at host pointer `host_vb` (stride `stride_bytes`) through our OWN host Vulkan
// pipeline into an offscreen target, read back, and return the count of
// non-background pixels rendered. Returns -1 on any failure (logged). Touches no
// rexglue state. Allocates and frees everything per call.
long RenderOneDrawA1(const void *host_vb, uint32_t vertex_count, uint32_t stride_bytes);

// A2 is implemented inside rexglue on the GPU command-processor thread (patch 0030),
// not here -- binding rexglue's live transient descriptor sets from another thread
// races their per-submission recycling. The title only sets the wos_native_a2 cvar.

}  // namespace native
}  // namespace wos
