Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState      g_SamplerDescriptorHeap[]   : register(s0, space1);

#ifdef __spirv__
struct SlotData
{
    [[vk::offset(16)]] uint textureSlot;
    uint samplerSlot;
};
[[vk::push_constant]] ConstantBuffer<SlotData> g_Slots : register(b1, space2);
#define kTextureSlot g_Slots.textureSlot
#define kSamplerSlot g_Slots.samplerSlot
#else
cbuffer Slots : register(b1, space2)
{
    uint kTextureSlot;
    uint kSamplerSlot;
};
#endif

struct PS_IN
{
    float4 position : SV_Position;
    float4 color    : COLOR0;
    float2 texCoord : TEXCOORD0;
};

float4 main(PS_IN input) : SV_Target
{
    if (kTextureSlot == 0xFFFFFFFFu)
        return input.color;
    float4 texel = g_Texture2DDescriptorHeap[kTextureSlot].SampleLevel(
        g_SamplerDescriptorHeap[kSamplerSlot], input.texCoord, 0.0);
    return texel * input.color;
}
