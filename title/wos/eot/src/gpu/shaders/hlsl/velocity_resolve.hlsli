#pragma once
#include "copy_common.hlsli"

Texture2DMS<float4, SAMPLE_COUNT> g_Texture2DMSDescriptorHeap[] : register(t0, space0);

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2DMS<float4, SAMPLE_COUNT> tex =
        g_Texture2DMSDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    const int2 p = int2(position.xy);
    float2 best = tex.Load(p, 0).xy;
    [unroll] for (int i = 1; i < SAMPLE_COUNT; ++i)
    {
        const float2 v = tex.Load(p, i).xy;
        if (abs(best.x) > 4.0 && abs(v.x) <= 4.0)
            best = v;
    }
    return float4(best, 0.0, 0.0);
}
