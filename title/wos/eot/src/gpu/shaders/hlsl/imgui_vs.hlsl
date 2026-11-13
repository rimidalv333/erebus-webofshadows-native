struct VS_IN
{
    float2 position : POSITION;
    float2 texCoord : TEXCOORD0;
    float4 color    : COLOR0;
};

struct VS_OUT
{
    float4 position : SV_Position;
    float4 color    : COLOR0;
    float2 texCoord : TEXCOORD0;
};

#ifdef __spirv__
struct OrthoData
{
    float2 scale;
    float2 translate;
};
[[vk::push_constant]] ConstantBuffer<OrthoData> g_Ortho : register(b0, space2);
#define kScale     g_Ortho.scale
#define kTranslate g_Ortho.translate
#else
cbuffer Ortho : register(b0, space2)
{
    float2 kScale;
    float2 kTranslate;
};
#endif

VS_OUT main(VS_IN input)
{
    VS_OUT output;
    output.position = float4(input.position * kScale + kTranslate, 0.0, 1.0);
    output.color = input.color;
    output.texCoord = input.texCoord;
    return output;
}
