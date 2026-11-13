#pragma once
#include "copy_common.hlsli"

Texture2DMS<float, SAMPLE_COUNT> g_Texture2DMSDescriptorHeap[] : register(t0, space0);

float main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Depth
{
    Texture2DMS<float, SAMPLE_COUNT> tex =
        g_Texture2DMSDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint w, h, samples;
    tex.GetDimensions(w, h, samples);
    const float2 uv = lerp(g_PushConstants.SourceRect.xy, g_PushConstants.SourceRect.zw, texCoord);
    const int2 c = clamp(int2(uv * float2(w, h)), int2(0, 0), int2(w, h) - 1);
    return tex.Load(c, 0);
}
