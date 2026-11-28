Texture2D<float4> Scene : register(t0);
Texture2D<float2> Motion : register(t1);
Texture2D<float4> WheelMask : register(t2);
Texture2D<float> WheelDepth : register(t3);
Texture2D<float> SceneDepth : register(t4);

struct WheelGpuConstants
{
    row_major float4x4 world;
    row_major float4x4 current;
    row_major float4x4 correctedPrevious;
    row_major float4x4 stockPrevious;
    row_major float4x4 inverseCurrent;
    float roll;
    uint frameId, canonicalId, valid;
    uint renderWidth, renderHeight, poolIndex, reserved;
    float4 padding[10];
};

StructuredBuffer<WheelGpuConstants> WheelData : register(t5);
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
    uint2 wheelMaskTextureSize;
    uint arcEnabled, arcFrame, arcCount, arcReserved;
    float2 jitterUV;
    uint2 sceneDepthTextureSize;
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

bool Finite4(float4 value)
{
    return all((asuint(value) & 0x7f800000u) != 0x7f800000u);
}

bool ReadWheelSurface(float2 outputUV, out uint index, out uint2 pixel, out float depth)
{
    index = 0u;
    pixel = 0u;
    depth = 0.0;
    // The color is in unjittered FSR output space. The two auxiliary
    // textures and the final depth were rasterized at render resolution.
    const float2 renderUV = outputUV + jitterUV;
    if (!Finite4(float4(renderUV, 0.0, 0.0)) || !all(renderUV >= 0.0) || !all(renderUV < 1.0))
        return false;
    pixel = min(uint2(renderUV * float2(motionActiveSize)), motionActiveSize - 1u);

    const float2 center = float2(pixel) + 0.5;
    const float2 auxUV = center / float2(wheelMaskTextureSize);
    const float id = WheelMask.SampleLevel(PointSampler, auxUV, 0.0).a * 65536.0;
    if (!Finite4(float4(id, 0.0, 0.0, 0.0)) || !(id >= 1.0 && id <= float(min(arcCount, 65536u))))
        return false;
    const uint encoded = uint(id);
    if (float(encoded) != id)
        return false;
    index = encoded - 1u;

    depth = WheelDepth.SampleLevel(PointSampler, auxUV, 0.0);
    const float sceneZ = SceneDepth.SampleLevel(PointSampler, center / float2(sceneDepthTextureSize), 0.0);
    if (!Finite4(float4(depth, sceneZ, 0.0, 0.0)) || !(depth >= 0.0 && depth <= 1.0 && sceneZ >= 0.0 && sceneZ <= 1.0))
        return false;
    // Both depths are in the same D3D projection. A different final surface
    // can cover this wheel after the auxiliary pass has recorded it.
    const float tolerance = max(1e-7, 2e-7 * max(abs(depth), abs(sceneZ)));
    return abs(depth - sceneZ) <= tolerance;
}

bool WheelIdentity(uint index, out uint canonicalId)
{
    canonicalId = WheelData[index].canonicalId;
    return canonicalId != 0u && WheelData[index].valid == 1u && WheelData[index].frameId == arcFrame &&
           WheelData[index].poolIndex == index && WheelData[index].renderWidth == motionActiveSize.x &&
           WheelData[index].renderHeight == motionActiveSize.y;
}

bool ReconstructWheelPoint(uint index, float2 rasterUV, float depth, out float3 local)
{
    local = 0.0;
    const float4 ndc = float4(2.0 * rasterUV.x - 1.0, 1.0 - 2.0 * rasterUV.y, depth, 1.0);
    const float4 localHomogeneous = mul(ndc, WheelData[index].inverseCurrent);
    if (!Finite4(localHomogeneous) || localHomogeneous.w <= 1e-7)
        return false;
    local = localHomogeneous.xyz / localHomogeneous.w;
    return Finite4(float4(local, 0.0));
}

bool ReadCorroboratedWheel(float2 outputUV, uint requiredCanonicalId,
                           out uint index, out uint canonicalId, out float3 local)
{
    canonicalId = 0u;
    local = 0.0;
    uint2 pixel;
    float depth;
    if (!ReadWheelSurface(outputUV, index, pixel, depth) || !WheelIdentity(index, canonicalId) ||
        (requiredCanonicalId != 0u && canonicalId != requiredCanonicalId))
        return false;

    // Both the reconstruction and native vectors refer to this same jittered
    // raster texel. Native SV_Target4 stores previous minus current position.
    const float2 rasterCenter = float2(pixel) + 0.5;
    const float2 renderSize = float2(motionActiveSize);
    const float2 rasterUV = rasterCenter / renderSize;
    if (!ReconstructWheelPoint(index, rasterUV, depth, local))
        return false;
    const float4 previous = mul(float4(local, 1.0), WheelData[index].correctedPrevious);
    if (!Finite4(previous) || previous.w <= 1e-7)
        return false;
    const float2 previousUV = 0.5 + float2(0.5, -0.5) * (previous.xy / previous.w);
    const float2 expected = (previousUV - rasterUV) * renderSize;
    const float2 observedUV = Motion.SampleLevel(PointSampler, rasterCenter / float2(motionTextureSize), 0.0) * rawToUV;
    if (!Finite4(float4(expected, observedUV)) || !all(abs(observedUV) <= 4.0))
        return false;

    // This checks which draw produced a surface at this texel. Its tolerance
    // includes float projection error and up to half an RG16F vector step.
    const float2 tolerance = 0.25 + 0.0005 * abs(expected);
    return all(abs(observedUV * renderSize - expected) <= tolerance);
}

bool ProjectWheelUV(uint index, float3 local, float roll, float time, out float2 uv)
{
    uv = 0.0;
    float s, c;
    sincos(time * roll, s, c);
    const float4 rotated = float4(local.x, c * local.y - s * local.z, s * local.y + c * local.z, 1.0);
    const float4 now = mul(rotated, WheelData[index].current);
    const float4 prior = mul(rotated, WheelData[index].stockPrevious);
    const float4 clip = lerp(now, prior, time);
    if (!Finite4(clip) || clip.w <= 1e-7)
        return false;
    uv = 0.5 + float2(0.5, -0.5) * (clip.xy / clip.w);
    return Finite4(float4(uv, 0.0, 0.0));
}

bool ArcDisplacement(uint index, float3 local, float roll, float2 projectedCenter,
                     float time, float radiusSquared, out float2 offsetUV)
{
    offsetUV = 0.0;
    float2 projected;
    if (!ProjectWheelUV(index, local, roll, time, projected))
        return false;
    offsetUV = projected - projectedCenter;
    const float2 offsetPx = offsetUV * float2(outputSize);
    return dot(offsetPx, offsetPx) <= radiusSquared;
}

float CappedArcTime(uint index, float3 local, float roll, float2 projectedCenter,
                    float signedHalfTime, float radiusSquared)
{
    float2 offsetUV;
    if (ArcDisplacement(index, local, roll, projectedCenter, signedHalfTime, radiusSquared, offsetUV))
        return abs(signedHalfTime);

    // Shorten the shutter on the curve instead of moving taps toward the
    // center along a chord. Each final tap independently checks the cap.
    float accepted = 0.0, rejected = 1.0;
    [loop]
    for (uint step = 0u; step < 7u; ++step)
    {
        const float candidate = (accepted + rejected) * 0.5;
        if (ArcDisplacement(index, local, roll, projectedCenter, signedHalfTime * candidate, radiusSquared, offsetUV))
            accepted = candidate;
        else
            rejected = candidate;
    }
    return abs(signedHalfTime) * accepted;
}

void AccumulateWheelBilinear(float2 tapUV, uint canonicalId, float weight,
                             inout float3 colorSum, inout float weightSum)
{
    const float2 pixel = tapUV * float2(outputSize) - 0.5;
    const int2 base = int2(floor(pixel));
    const float2 fraction = pixel - float2(base);
    // Validate each contribution's actual output texel. At a spoke/cavity
    // edge, even ordinary scene bilinear filtering could otherwise leak.
    [unroll]
    for (uint corner = 0u; corner < 4u; ++corner)
    {
        const uint2 cornerOffset = uint2(corner & 1u, corner >> 1u);
        const float2 components = lerp(1.0 - fraction, fraction, float2(cornerOffset));
        const float bilinear = weight * components.x * components.y;
        const int2 samplePixel = base + int2(cornerOffset);
        if (bilinear <= 0.0 || any(samplePixel < 0) || any(samplePixel >= int2(outputSize)))
            continue;
        const float2 sampleUV = (float2(samplePixel) + 0.5) / float2(outputSize);
        uint tapIndex, tapCanonical;
        float3 tapLocal;
        if (!ReadCorroboratedWheel(sampleUV, canonicalId, tapIndex, tapCanonical, tapLocal))
            continue;
        colorSum += bilinear * Scene.SampleLevel(PointSampler, sampleUV, 0.0).rgb;
        weightSum += bilinear;
    }
}

bool TryBlurWheel(float4 center, float2 uv, float maxRadiusPx, float exposureFraction, out float4 result)
{
    result = center;
    if (arcFrame == 0u || arcCount == 0u || any(wheelMaskTextureSize < motionActiveSize) ||
        any(sceneDepthTextureSize < motionActiveSize))
        return false;
    uint index, canonicalId;
    float3 local;
    if (!ReadCorroboratedWheel(uv, 0u, index, canonicalId, local))
        return false;

    const float roll = WheelData[index].roll;
    float2 projectedCenter;
    if (!Finite4(float4(roll, 0.0, 0.0, 0.0)) || !(abs(roll) > 0.0 && abs(roll) <= 3.141593) ||
        !ProjectWheelUV(index, local, roll, 0.0, projectedCenter))
        return false;

    const float radiusSquared = maxRadiusPx * maxRadiusPx;
    const float shutterHalfTime = exposureFraction * 0.5;
    const float halfTime = min(CappedArcTime(index, local, roll, projectedCenter, -shutterHalfTime, radiusSquared),
                               CappedArcTime(index, local, roll, projectedCenter, shutterHalfTime, radiusSquared));
    if (halfTime <= 0.0)
        return true;

    float2 endForward, endBackward;
    ArcDisplacement(index, local, roll, projectedCenter, halfTime, radiusSquared, endForward);
    ArcDisplacement(index, local, roll, projectedCenter, -halfTime, radiusSquared, endBackward);
    const float2 size = float2(outputSize);
    const float radiusPx = min(maxRadiusPx, max(length(endForward * size), length(endBackward * size)));
    const float strength = saturate((radiusPx - 0.5) * 2.0);
    if (strength <= 0.0)
        return true;

    const int halfSamples = int(clamp(sampleCount / 2u, 1u, 8u));
    const float2 halfTexel = 0.5 / size;
    float3 colorSum = center.rgb;
    float weightSum = 1.0;
    [loop]
    for (int tap = -halfSamples; tap <= halfSamples; ++tap)
    {
        if (tap == 0)
            continue;
        const float time = halfTime * float(tap) / float(halfSamples);
        float2 offsetUV;
        if (!ArcDisplacement(index, local, roll, projectedCenter, time, radiusSquared, offsetUV))
            continue;
        const float2 tapUV = uv + offsetUV;
        if (any(tapUV < halfTexel) || any(tapUV > 1.0 - halfTexel))
            continue;
        const float weight = abs(tap) == halfSamples ? 0.5 : 1.0;
        AccumulateWheelBilinear(tapUV, canonicalId, weight, colorSum, weightSum);
    }
    if (weightSum > 1.0)
        result = float4(lerp(center.rgb, colorSum / weightSum, strength), center.a);
    return true;
}

float4 BlurLinear(float4 center, float2 uv, float maxRadiusPx, float exposureFraction)
{
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

    [branch]
    if (arcEnabled != 0u)
    {
        float4 wheel;
        if (TryBlurWheel(center, uv, maxRadiusPx, exposureFraction, wheel))
            return wheel;
    }
    return BlurLinear(center, uv, maxRadiusPx, exposureFraction);
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
