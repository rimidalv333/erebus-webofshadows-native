#pragma once
#include "copy_common.hlsli"

Texture2DMS<float4, SAMPLE_COUNT> g_Texture2DMSDescriptorHeap[] : register(t0, space0);

float SelectChannel(float4 v, uint sel)
{
    return sel < 4u ? v[sel] : (sel == 5u ? 1.0 : 0.0);
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2DMS<float4, SAMPLE_COUNT> tex =
        g_Texture2DMSDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint w, h, samples;
    tex.GetDimensions(w, h, samples);
    const float2 uv = lerp(g_PushConstants.SourceRect.xy, g_PushConstants.SourceRect.zw, texCoord);
    const float2 p = uv * float2(w, h);
    const float2 texels_per_pixel = float2(ddx(uv.x) * w, ddy(uv.y) * h);
    const uint taps = clamp((uint)(max(texels_per_pixel.x, texels_per_pixel.y) + 0.5), 1u, 4u);
    const int2 base = int2(floor(p - float(taps) * 0.5 + 0.5));
    const int2 last = int2(w, h) - 1;
    float4 acc = 0.0;
    [loop] for (uint y = 0u; y < taps; ++y)
    {
        [loop] for (uint x = 0u; x < taps; ++x)
        {
            const int2 c = clamp(base + int2(x, y), int2(0, 0), last);
            [unroll] for (int i = 0; i < SAMPLE_COUNT; ++i)
                acc += tex.Load(c, i);
        }
    }
    float4 s = acc / (float)(taps * taps * SAMPLE_COUNT);
    if (g_PushConstants.Param1 < 0.5 && (g_PushConstants.ResourceDescriptorIndex2 & 0x80000000u))
    {
        const uint perm = g_PushConstants.ResourceDescriptorIndex2;
        const float4 src = s;
        s = float4(SelectChannel(src, perm & 7u), SelectChannel(src, (perm >> 3) & 7u),
                   SelectChannel(src, (perm >> 6) & 7u), SelectChannel(src, (perm >> 9) & 7u));
    }
    const float a = g_PushConstants.Param1 > 0.5 ? 1.0 : s.a;
    return float4(s.rgb * g_PushConstants.Param0, a);
}
