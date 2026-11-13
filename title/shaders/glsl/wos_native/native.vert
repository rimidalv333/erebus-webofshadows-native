#version 450 core

// WebOfShadowsRecomp native renderer -- A1 plumbing-proof vertex shader.
//
// This is NOT a translation of the game's shader. It is a deliberately trivial
// passthrough whose only job is to prove the A1 plumbing end to end: that we can
// take ONE real draw's vertices out of guest memory, lay them out exactly as the
// game's vertex-fetch constant describes, feed them through a host Vulkan pipeline
// on the shared device, and get pixels into an offscreen target we can read back.
//
// Vertex layout for the first target draw (VS 1E6883FC, [WoS-NATIVE-VTX]):
//   stride 7 words (28 bytes)
//   attr0: 32_32_32_FLOAT    at offset 0  -> position (object space)
//   attr1: 32_32_32_32_FLOAT at offset 3w -> treated here as a color/extra vec4
// We pass position straight through in clip space (the real draw's positions for
// the simple dynamic-geometry VS are already roughly screen/clip space for UI-ish
// passes; A1 only needs *some* geometry to rasterize, not correctness of transform).
layout (location = 0) in vec3 inPos;
layout (location = 1) in vec4 inExtra;

layout (location = 0) out vec4 outExtra;

out gl_PerVertex {
    vec4 gl_Position;
};

void main(void) {
    // Pass through. If positions are outside clip space the triangle is simply
    // clipped; the pixel-count readback still tells us whether ANYTHING rasterized,
    // which is the A1 signal. (A2 will apply the real transform constants.)
    gl_Position = vec4(inPos, 1.0);
    outExtra = inExtra;
}
