#pragma once
#include "copy_common.hlsli"

Texture2DMS<float, SAMPLE_COUNT> g_DepthHeap[] : register(t0, space0);
Texture2DMS<uint2, SAMPLE_COUNT> g_StencilHeap[] : register(t0, space1);

struct Output
{
    float depth : SV_Depth;
    uint stencil : SV_StencilRef;
};

Output main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD)
{
    Texture2DMS<float, SAMPLE_COUNT> depth = g_DepthHeap[g_PushConstants.ResourceDescriptorIndex];
    Texture2DMS<uint2, SAMPLE_COUNT> stencil = g_StencilHeap[g_PushConstants.ResourceDescriptorIndex2];
    uint w, h, samples;
    depth.GetDimensions(w, h, samples);
    const float2 uv = lerp(g_PushConstants.SourceRect.xy, g_PushConstants.SourceRect.zw, texCoord);
    const int2 c = clamp(int2(uv * float2(w, h)), int2(0, 0), int2(w, h) - 1);
    Output o;
    o.depth = depth.Load(c, 0);
    o.stencil = stencil.Load(c, 0).g;
    return o;
}
