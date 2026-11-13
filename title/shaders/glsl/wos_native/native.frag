#version 450 core

// WebOfShadowsRecomp native renderer -- A1 plumbing-proof fragment shader.
// Flat, unmistakable color so a readback can count "native-rendered" pixels
// against a known clear color. Not the game's shading; A1 proves the pipeline,
// not the look.
layout (location = 0) in vec4 inExtra;
layout (location = 0) out vec4 outColor;

void main(void) {
    // Solid magenta -- distinct from the clear color (black) so rendered pixels
    // are trivially counted on readback.
    outColor = vec4(1.0, 0.0, 1.0, 1.0);
}
