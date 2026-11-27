Texture2D<float4> Scene : register(t0);
Texture2D<float2> Motion : register(t1);
RWTexture2D<float4> Result : register(u0);
SamplerState SceneSampler : register(s0);
SamplerState PointSampler : register(s1);

cbuffer BlurConstants : register(b0)
{
    uint2 outputSize;
    uint2 motionActiveSize;
    float2 rawToUV;
    float frameTimeMs;
    float exposureValue;
    uint exposureMode;
    uint sampleCount;
    float maxRadius;
    uint displayMode;
    uint2 motionTextureSize;
    uint2 padding;
};

bool ReadMotionUV(float2 uv, out float2 motion)
{
    // FSR's active motion viewport starts at texel (0, 0) and can be smaller
    // than the allocation. Use point sampling within the active extent.
    const uint2 pixel = min(uint2(uv * float2(motionActiveSize)), motionActiveSize - 1u);
    const float2 motionUV = (float2(pixel) + 0.5) / float2(motionTextureSize);
    motion = Motion.SampleLevel(PointSampler, motionUV, 0.0) * rawToUV;

    // Reject NaN, infinity and implausible vectors before limiting the radius.
    return all((asuint(motion) & 0x7f800000u) != 0x7f800000u) && all(abs(motion) <= 4.0);
}

float4 BlurPixel(float4 center, float2 uv)
{
    const float maxRadiusPx = clamp(maxRadius, 0.0, 32.0);
    if (maxRadiusPx <= 0.0 || (displayMode == 1u && uv.x < 0.5) ||
        !(frameTimeMs > 0.0 && frameTimeMs <= 1000.0) ||
        any(motionActiveSize == 0u) || any(motionActiveSize > motionTextureSize))
        return center;

    const float exposureFraction = exposureMode == 1u
        ? saturate(clamp(exposureValue, 0.0, 24.0) / frameTimeMs)
        : saturate(exposureValue / 360.0);
    if (exposureFraction <= 0.0)
        return center;

    float2 motion;
    if (!ReadMotionUV(uv, motion))
        return center;

    const float2 size = float2(outputSize);
    const float2 halfTexel = 0.5 / size;
    const float2 motionPx = motion * size;
    const float motionLengthPx = length(motionPx);
    const float radiusPx = min(0.5 * motionLengthPx * exposureFraction, maxRadiusPx);
    const float strength = saturate((radiusPx - 0.5) * 2.0);
    if (strength <= 0.0)
        return center;

    const float2 radiusUV = motion * (radiusPx / motionLengthPx);
    const int halfSamples = int(clamp(sampleCount / 2u, 1u, 8u));
    const float tolerancePx = max(1.0, 0.25 * motionLengthPx);
    const float toleranceSquared = tolerancePx * tolerancePx;
    float3 colorSum = center.rgb;
    float weightSum = 1.0;
    [loop]
    for (int tap = -halfSamples; tap <= halfSamples; ++tap)
    {
        if (tap == 0)
            continue;

        const float along = float(tap) / float(halfSamples);
        const float2 tapUV = uv + radiusUV * along;
        if (any(tapUV < halfTexel) || any(tapUV > 1.0 - halfTexel))
            continue;

        float2 tapMotion;
        if (!ReadMotionUV(tapUV, tapMotion))
            continue;

        const float2 differencePx = tapMotion * size - motionPx;
        const float agreement = 1.0 - smoothstep(0.25 * toleranceSquared, toleranceSquared,
            dot(differencePx, differencePx));
        if (agreement <= 0.0)
            continue;

        const float weight = agreement * (abs(tap) == halfSamples ? 0.5 : 1.0);
        colorSum += weight * Scene.SampleLevel(SceneSampler, tapUV, 0.0).rgb;
        weightSum += weight;
    }

    if (weightSum <= 1.0)
        return center;
    return float4(lerp(center.rgb, colorSum / weightSum, strength), center.a);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchId.xy;
    if (any(pixel >= outputSize))
        return;

    const float2 uv = (float2(pixel) + 0.5) / float2(outputSize);
    Result[pixel] = BlurPixel(Scene.SampleLevel(PointSampler, uv, 0.0), uv);
}
