#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState      g_SamplerDescriptorHeap[]   : register(s0, space3);

float SelectChannel(float4 v, uint sel)
{
    return sel < 4u ? v[sel] : (sel == 5u ? 1.0 : 0.0);
}

float Lanczos2(float x)
{
    x = abs(x);
    if (x < 1e-4)
        return 1.0;
    if (x >= 2.0)
        return 0.0;
    const float px = 3.14159265 * x;
    return (sin(px) / px) * (sin(px * 0.5) / (px * 0.5));
}

float4 BoxDownsample(Texture2D<float4> tex, float2 uv, uint2 size, uint ratio)
{
    const uint taps = min(ratio, 8u);
    const float2 p = uv * float2(size);
    const int2 base = int2(floor(p - float(ratio) * 0.5 + 0.5));
    const int2 last = int2(size) - 1;
    float4 acc = 0.0;
    [loop] for (uint y = 0u; y < taps; ++y)
    {
        const int oy = (int)((y * ratio + ratio / 2u) / taps);
        [loop] for (uint x = 0u; x < taps; ++x)
        {
            const int ox = (int)((x * ratio + ratio / 2u) / taps);
            acc += tex.Load(int3(clamp(base + int2(ox, oy), int2(0, 0), last), 0));
        }
    }
    return acc / (float)(taps * taps);
}

float4 LanczosWeights(float f)
{
    f = clamp(f, 1e-4, 1.0 - 1e-4);
    float sf, cf, sh, ch;
    sincos(3.14159265 * f, sf, cf);
    sincos(1.57079633 * f, sh, ch);
    const float a0 = f + 1.0, a1 = f, a2 = 1.0 - f, a3 = 2.0 - f;
    return float4(-sf * ch / (a0 * a0), sf * sh / (a1 * a1), sf * ch / (a2 * a2),
                  -sf * sh / (a3 * a3));
}

float4 LanczosUpsample(Texture2D<float4> tex, SamplerState linear_sampler, float2 uv, float2 size)
{
    const float2 p = uv * size - 0.5;
    const float2 f = frac(p);
    const float2 base = floor(p);
    const float4 wx = LanczosWeights(f.x), wy = LanczosWeights(f.y);
    const float2 w0 = float2(wx.x, wy.x);
    const float2 w1 = float2(wx.y, wy.y);
    const float2 w2 = float2(wx.z, wy.z);
    const float2 w3 = float2(wx.w, wy.w);
    const float2 w12 = w1 + w2;
    const float2 t0 = (base - 0.5) / size;
    const float2 t12 = (base + 0.5 + w2 / w12) / size;
    const float2 t3 = (base + 2.5) / size;
    float4 acc = 0.0;
    acc += tex.SampleLevel(linear_sampler, float2(t0.x, t0.y), 0) * (w0.x * w0.y);
    acc += tex.SampleLevel(linear_sampler, float2(t12.x, t0.y), 0) * (w12.x * w0.y);
    acc += tex.SampleLevel(linear_sampler, float2(t3.x, t0.y), 0) * (w3.x * w0.y);
    acc += tex.SampleLevel(linear_sampler, float2(t0.x, t12.y), 0) * (w0.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(t12.x, t12.y), 0) * (w12.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(t3.x, t12.y), 0) * (w3.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(t0.x, t3.y), 0) * (w0.x * w3.y);
    acc += tex.SampleLevel(linear_sampler, float2(t12.x, t3.y), 0) * (w12.x * w3.y);
    acc += tex.SampleLevel(linear_sampler, float2(t3.x, t3.y), 0) * (w3.x * w3.y);
    const float2 sum = w0 + w12 + w3;
    return acc / (sum.x * sum.y);
}

float4 CatmullRomUpsample(Texture2D<float4> tex, SamplerState linear_sampler, float2 uv, float2 size)
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
    float4 acc = 0.0;
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos12.x, tex_pos0.y), 0) * (w12.x * w0.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos0.x, tex_pos12.y), 0) * (w0.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos12.x, tex_pos12.y), 0) * (w12.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos3.x, tex_pos12.y), 0) * (w3.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos12.x, tex_pos3.y), 0) * (w12.x * w3.y);
    const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return acc / weight;
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    float2 uv = lerp(g_PushConstants.SourceRect.xy, g_PushConstants.SourceRect.zw, texCoord);
    uint w, h;
    tex.GetDimensions(w, h);
    float4 s;
    const uint mode = (uint)(g_PushConstants.Extra.x + 0.5);
    if (mode == 1u)
    {
        s = BoxDownsample(tex, uv, uint2(w, h), max((uint)(g_PushConstants.Extra.y + 0.5), 2u));
    }
    else if (mode == 2u)
    {
        s = LanczosUpsample(tex, g_SamplerDescriptorHeap[0u], uv, float2(w, h));
    }
    else if (mode == 3u)
    {
        s = CatmullRomUpsample(tex, g_SamplerDescriptorHeap[0u], uv, float2(w, h));
    }
    else
    {
        float2 texels_per_pixel = float2(ddx(uv.x) * w, ddy(uv.y) * h);
        bool one_to_one = all(abs(texels_per_pixel - 1.0) < 0.01);
        s = tex.Sample(g_SamplerDescriptorHeap[one_to_one ? 1u : 0u], uv);
    }
    if (g_PushConstants.Param1 < 0.5 && (g_PushConstants.ResourceDescriptorIndex2 & 0x80000000u))
    {
        uint p = g_PushConstants.ResourceDescriptorIndex2;
        float4 src = s;
        s = float4(SelectChannel(src, p & 7u), SelectChannel(src, (p >> 3) & 7u),
                   SelectChannel(src, (p >> 6) & 7u), SelectChannel(src, (p >> 9) & 7u));
    }
    float3 rgb = s.rgb * g_PushConstants.Param0;
    if (g_PushConstants.ColorAdjust.w > 0.0)
    {
        float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
        rgb = lerp(float3(luma, luma, luma), rgb, g_PushConstants.ColorAdjust.z);
        rgb = (rgb - 0.5) * g_PushConstants.ColorAdjust.y + 0.5 + g_PushConstants.ColorAdjust.x;
        rgb = pow(saturate(rgb), 1.0 / g_PushConstants.ColorAdjust.w);
    }
    if (g_PushConstants.Param1 > 1.5)
    {
        Texture2D<float4> lut = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex2];
        float3 u = saturate(rgb) * (1023.0 / 1024.0) + (0.5 / 1024.0);
        rgb = float3(lut.SampleLevel(g_SamplerDescriptorHeap[0], float2(u.r, 0.5), 0).r,
                     lut.SampleLevel(g_SamplerDescriptorHeap[0], float2(u.g, 0.5), 0).g,
                     lut.SampleLevel(g_SamplerDescriptorHeap[0], float2(u.b, 0.5), 0).b);
    }
    float a = g_PushConstants.Param1 > 0.5 ? 1.0 : s.a;
    // WoS port: Extra.z = a resolve out of a 7e3 (2_10_10_10_FLOAT) render target. The
    // Xenos format holds 0..31.875 and no NaN or infinity, so whatever a shader wrote was
    // clamped there; the host keeps it in half floats, NaN and Inf included. Clamped here
    // as the EDRAM would have (NaN to 0), and the 2-bit alpha to 0..1.
    if (g_PushConstants.Extra.z > 1.5)
    {
        // (Extra.z = 2: the 16-bit fixed-point formats, -32..32 in every channel)
        rgb = select(isnan(rgb), float3(0.0, 0.0, 0.0), clamp(rgb, -32.0, 32.0));
        a = isnan(a) ? 0.0 : clamp(a, -32.0, 32.0);
    }
    else if (g_PushConstants.Extra.z > 0.5)
    {
        rgb = select(isnan(rgb), float3(0.0, 0.0, 0.0), clamp(rgb, 0.0, 31.875));
        a = isnan(a) ? 0.0 : saturate(a);
    }
    return float4(rgb, a);
}
