#include "copy_common.hlsli"

Texture2D<float> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

float main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Depth
{
    float2 uv = lerp(g_PushConstants.SourceRect.xy, g_PushConstants.SourceRect.zw, texCoord);
    return g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
        .Sample(g_SamplerDescriptorHeap[1], uv);
}
