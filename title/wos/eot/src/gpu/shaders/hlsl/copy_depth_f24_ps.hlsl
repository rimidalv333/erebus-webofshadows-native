// WoS port (fix67): copy_depth_ps for a depth surface the guest keeps as D24FS8 (20e4 float).
// The Xbox 360 stores such depth with a 20-bit mantissa, truncated (Xenia's default). Shaders
// that compare against a resolved shadow map (the city's facade LOD vs its own building's shell)
// rely on that: the stored depth rounds down, the receiver's own depth wins the >= test. A full
// D32 value can land a hair above it - the facades shadowed themselves in patches that moved
// with the shadow cascade (the camera's pitch).
#include "copy_common.hlsli"

Texture2D<float> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

float Truncate20e4(float d)
{
    const uint u = asuint(d);
    if (u >= 0x38800000u)              // normalized 20e4: drop the 3 low float32 mantissa bits
        return asfloat(u & ~7u);
    return floor(max(d, 0.0) * 17179869184.0) / 17179869184.0;  // denormal 20e4: 2^-34 steps
}

float main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Depth
{
    float2 uv = lerp(g_PushConstants.SourceRect.xy, g_PushConstants.SourceRect.zw, texCoord);
    return Truncate20e4(g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
        .Sample(g_SamplerDescriptorHeap[1], uv));
}
