Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState      g_SamplerDescriptorHeap[]   : register(s0, space3);

struct TaaConstants
{
    row_major float4x4 Reproject;
    float4 Jitter;
    float4 Params;
    uint4  Indices;
    float4 Motion;
    uint4  Indices2;
};

#ifdef __spirv__
struct PushConstants
{
    uint64_t Constants;
    uint64_t Unused1;
    uint64_t Unused2;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
#define TAA_LOAD(type, off) vk::RawBufferLoad<type>(g_PushConstants.Constants + (off))
#define TAA_REPROJECT_ROW(r) TAA_LOAD(float4, (r) * 16)
#define TAA_JITTER  TAA_LOAD(float4, 64)
#define TAA_PARAMS  TAA_LOAD(float4, 80)
#define TAA_INDICES TAA_LOAD(uint4, 96)
#define TAA_MOTION  TAA_LOAD(float4, 112)
#define TAA_INDICES2 TAA_LOAD(uint4, 128)
#else
ConstantBuffer<TaaConstants> g_Taa : register(b0, space4);
#define TAA_REPROJECT_ROW(r) g_Taa.Reproject[r]
#define TAA_JITTER  g_Taa.Jitter
#define TAA_PARAMS  g_Taa.Params
#define TAA_INDICES g_Taa.Indices
#define TAA_MOTION  g_Taa.Motion
#define TAA_INDICES2 g_Taa.Indices2
#endif

static const uint kSamplerLinearClamp = 0u;

float Luma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

float3 RgbToYCoCg(float3 c)
{
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                  0.5 * c.r - 0.5 * c.b,
                  -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 YCoCgToRgb(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

float3 ClipToBox(float3 boxMin, float3 boxMax, float3 p)
{
    const float3 centre = 0.5 * (boxMax + boxMin);
    const float3 extent = 0.5 * (boxMax - boxMin) + 1e-5;
    const float3 offset = p - centre;
    const float3 unit = abs(offset / extent);
    const float maxUnit = max(unit.x, max(unit.y, unit.z));
    return maxUnit > 1.0 ? centre + offset / maxUnit : p;
}

float3 SampleHistory(Texture2D<float4> tex, SamplerState linear_sampler, float2 uv, float2 size)
{
    const float2 sample_pos = uv * size;
    const float2 tex_pos1 = floor(sample_pos - 0.5) + 0.5;
    const float2 f = sample_pos - tex_pos1;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 offset12 = w2 / w12;
    const float2 tex_pos0 = (tex_pos1 - 1.0) / size;
    const float2 tex_pos3 = (tex_pos1 + 2.0) / size;
    const float2 tex_pos12 = (tex_pos1 + offset12) / size;
    float3 acc = 0.0;
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos12.x, tex_pos0.y), 0).rgb * (w12.x * w0.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos0.x, tex_pos12.y), 0).rgb * (w0.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos12.x, tex_pos12.y), 0).rgb * (w12.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos3.x, tex_pos12.y), 0).rgb * (w3.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos12.x, tex_pos3.y), 0).rgb * (w12.x * w3.y);
    const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(acc / weight, 0.0);
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    const uint4 indices = TAA_INDICES;
    const float4 params = TAA_PARAMS;
    const float4 jitter = TAA_JITTER;
    Texture2D<float4> scene = g_Texture2DDescriptorHeap[indices.x];
    Texture2D<float4> history = g_Texture2DDescriptorHeap[indices.y];
    Texture2D<float4> depth = g_Texture2DDescriptorHeap[indices.z];

    const int2 size = int2(params.zw);
    const int2 px = int2(position.xy);
    const float4 centre = scene.Load(int3(px, 0));

    const float2 jitterPx = jitter.xy * params.zw;
    float3 m1 = 0.0, m2 = 0.0;
    float3 filtered = 0.0;
    float wsum = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const int2 c = clamp(px + int2(x, y), int2(0, 0), size - 1);
            const float3 n = scene.Load(int3(c, 0)).rgb;
            const float3 ycc = RgbToYCoCg(n);
            m1 += ycc;
            m2 += ycc * ycc;
            const float2 d = float2(x, y) - jitterPx;
            const float w = exp(-2.29 * dot(d, d));
            filtered += n * w;
            wsum += w;
        }
    }
    const float4 current = float4(filtered / max(wsum, 1e-5), centre.a);
    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float gamma = 1.25;
    const float3 boxMin = mean - gamma * sigma;
    const float3 boxMax = mean + gamma * sigma;

    if (params.y < 0.5 || (indices.w & 1u) != 0u)
        return current;

    const uint4 indices2 = TAA_INDICES2;
    bool haveVector = false;
    float2 prevUv = 0.0;
    if (indices2.y != 0u)
    {
        const float2 v = g_Texture2DDescriptorHeap[indices2.x].Load(int3(px, 0)).xy;
        if (abs(v.x) < 4.0 && abs(v.y) < 4.0)
        {
            prevUv = texCoord - v;
            haveVector = true;
        }
    }
    if (!haveVector)
    {
        const float d = depth.Load(int3(px, 0)).x;
        const float4 clip = float4(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0, 1.0 - d, 1.0);
        float4 prev;
        prev.x = dot(clip, float4(TAA_REPROJECT_ROW(0).x, TAA_REPROJECT_ROW(1).x, TAA_REPROJECT_ROW(2).x, TAA_REPROJECT_ROW(3).x));
        prev.y = dot(clip, float4(TAA_REPROJECT_ROW(0).y, TAA_REPROJECT_ROW(1).y, TAA_REPROJECT_ROW(2).y, TAA_REPROJECT_ROW(3).y));
        prev.z = dot(clip, float4(TAA_REPROJECT_ROW(0).z, TAA_REPROJECT_ROW(1).z, TAA_REPROJECT_ROW(2).z, TAA_REPROJECT_ROW(3).z));
        prev.w = dot(clip, float4(TAA_REPROJECT_ROW(0).w, TAA_REPROJECT_ROW(1).w, TAA_REPROJECT_ROW(2).w, TAA_REPROJECT_ROW(3).w));
        if (prev.w <= 1e-6)
            return current;
        const float2 prevNdc = prev.xy / prev.w;
        prevUv = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
    }
    if (any(prevUv < 0.0) || any(prevUv > 1.0))
        return current;

    float3 hist = SampleHistory(history, g_SamplerDescriptorHeap[kSamplerLinearClamp], prevUv, float2(size));
    const float3 histYcc = RgbToYCoCg(hist);
    const float3 clippedYcc = ClipToBox(boxMin, boxMax, histYcc);
    hist = YCoCgToRgb(clippedYcc);

    const float motionPx = length((prevUv - texCoord) * params.zw);
    const float motionWeight = haveVector
        ? 1.0 - 0.5 * saturate(motionPx / max(8.0 * TAA_MOTION.x, 1.0))
        : 1.0 - saturate(motionPx / max(TAA_MOTION.x, 1.0));
    const float clipDistance = length((histYcc - clippedYcc) / (gamma * sigma + 1e-4));
    const float clipWeight = 1.0 - 0.5 * saturate(clipDistance);
    const float feedback = params.x * motionWeight * clipWeight;
    const float wc = (1.0 - feedback) / (1.0 + Luma(current.rgb));
    const float wh = feedback / (1.0 + Luma(hist));
    const float3 blended = (current.rgb * wc + hist * wh) / max(wc + wh, 1e-5);
    return float4(blended, current.a);
}
