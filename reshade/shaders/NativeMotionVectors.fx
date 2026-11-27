texture2D GameColor : COLOR;
texture2D NativeMotionVectors : GTAV_NATIVE_MOTION;

sampler2D GameColorSampler
{
    Texture = GameColor;
    AddressU = CLAMP;
    AddressV = CLAMP;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = POINT;
};

sampler2D NativeMotionSampler
{
    Texture = NativeMotionVectors;
    AddressU = CLAMP;
    AddressV = CLAMP;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = POINT;
};

// The add-on sets these values for the copied frame. Scale maps signed RG
// directly to render pixels. UVScale maps viewport UV to the top-left active
// region of the SRV. A tightly cropped copy has UVScale = (1, 1).
// Without initializers, ReShade leaves them writable in Performance Mode.
uniform bool NativeMotionValid < hidden = true; nosave = true; noreset = true; >;
uniform float2 NativeMotionScale < hidden = true; nosave = true; noreset = true; >;
uniform float2 NativeMotionRenderSize < hidden = true; nosave = true; noreset = true; >;
uniform float2 NativeMotionTextureSize < hidden = true; nosave = true; noreset = true; >;
uniform float2 NativeMotionUVScale < hidden = true; nosave = true; noreset = true; >;

uniform int NativeMotionDisplay <
    ui_type = "combo";
    ui_label = "Display";
    ui_items = "Direction and magnitude\0Signed X and Y\0";
    ui_tooltip = "Hue follows the vector clockwise from screen-right (red). In signed mode red is X, green is Y, and 0.5 is zero.";
> = 0;

uniform int NativeMotionUnits <
    ui_type = "combo";
    ui_label = "Vector units";
    ui_items = "Render pixels\0Raw texture RG\0Viewport UV\0";
    ui_tooltip = "Scaled FSR input points from the current frame to the previous frame, with positive Y down. Optional FSR jitter cancellation happens after this input.";
> = 0;

uniform float NativeMotionGain <
    ui_type = "drag";
    ui_label = "Gain";
    ui_min = 0.01;
    ui_max = 64.0;
    ui_step = 0.01;
    ui_tooltip = "At gain 1, the display saturates at 16 render pixels, one raw unit, or 0.01 viewport UV.";
> = 1.0;

uniform float NativeMotionOpacity <
    ui_type = "slider";
    ui_label = "Opacity";
    ui_min = 0.0;
    ui_max = 1.0;
> = 1.0;

uniform bool NativeMotionShowStatus <
    ui_label = "Show status marker";
    ui_tooltip = "Green indicates a fresh motion-vector frame. Red indicates missing or invalid input.";
> = true;

void NativeMotionVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
    position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

float3 NativeMotionDirectionColor(float2 motion)
{
    if (all(motion == 0.0))
        return float3(0.0, 0.0, 0.0);

    const float hue = frac(atan2(motion.y, motion.x) * 0.1591549430918953);
    const float3 wheel = abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return saturate(wheel - 1.0) * saturate(length(motion));
}

float4 NativeMotionPS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    const float4 game = tex2Dlod(GameColorSampler, float4(uv, 0.0, 0.0));
    const bool frame_valid = NativeMotionValid &&
        all(NativeMotionRenderSize >= 1.0) && all(NativeMotionTextureSize >= 1.0) &&
        all(NativeMotionUVScale >= 1.0 / NativeMotionTextureSize) && all(NativeMotionUVScale <= 1.0);

    // Leave the first text row clear for Steam's FPS counter.
    const float2 marker_offset = position.xy - float2(18.0, 48.0);
    const float marker_distance_squared = dot(marker_offset, marker_offset);
    if (NativeMotionShowStatus && marker_distance_squared < 49.0)
    {
        const float3 marker = frame_valid ? float3(0.18, 0.95, 0.42) : float3(1.0, 0.22, 0.2);
        return float4(marker_distance_squared < 25.0 ? marker : float3(0.06, 0.06, 0.06), game.a);
    }

    if (!frame_valid || NativeMotionOpacity <= 0.0)
        return game;

    const float2 half_texel = 0.5 / NativeMotionTextureSize;
    const float2 motion_uv = clamp(uv * NativeMotionUVScale, half_texel, NativeMotionUVScale - half_texel);
    const float2 raw = tex2Dlod(NativeMotionSampler, float4(motion_uv, 0.0, 0.0)).rg;

    float2 motion = raw;
    if (NativeMotionUnits != 1)
    {
        // FSR already accounts for the Y sign in the provided scale.
        motion *= NativeMotionScale;
        if (NativeMotionUnits == 2)
            motion *= 100.0 / NativeMotionRenderSize;
        else
            motion /= 16.0;
    }
    motion *= NativeMotionGain;
    // An all-ones IEEE 754 exponent identifies NaN or infinity. This also
    // works with Proton's HLSL compiler, which may lack isnan and isinf.
    if (any((asuint(motion) & 0x7f800000u) == 0x7f800000u))
        return game;

    const float3 display = NativeMotionDisplay == 1
        ? float3(saturate(0.5 + 0.5 * motion), 0.5)
        : NativeMotionDirectionColor(motion);

    return float4(lerp(game.rgb, display, NativeMotionOpacity), game.a);
}

technique NativeMotionVisualization < ui_label = "GTA V Native Motion Vectors"; >
{
    pass
    {
        VertexShader = NativeMotionVS;
        PixelShader = NativeMotionPS;
    }
}
