texture2D GameColor : COLOR;
texture2D NativeMotionVectors : GTAV_NATIVE_MOTION;

sampler2D GameColorSampler
{
    Texture = GameColor;
    AddressU = CLAMP;
    AddressV = CLAMP;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
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

// Compatibility metadata; filtering now happens in the native compute shader.
// Omitting initializers keeps them writable in ReShade Performance Mode.
uniform bool NativeMotionValid < hidden = true; nosave = true; noreset = true; >;
uniform float2 NativeMotionScale < hidden = true; nosave = true; noreset = true; >;
uniform float2 NativeMotionRenderSize < hidden = true; nosave = true; noreset = true; >;
uniform float2 NativeMotionTextureSize < hidden = true; nosave = true; noreset = true; >;
uniform float2 NativeMotionUVScale < hidden = true; nosave = true; noreset = true; >;
uniform float NativeMotionFrameTimeMs < hidden = true; nosave = true; noreset = true; >;

// The supplied preset supplies defaults. No initializers keeps controls readable
// by the add-on in ReShade Performance Mode as well as Standard Mode.
uniform int NativeMotionBlurExposureMode <
    ui_type = "combo";
    ui_label = "Exposure mode";
    ui_items = "Shutter angle\0Fixed exposure\0";
>;

uniform float NativeMotionBlurShutterAngle <
    ui_type = "slider";
    ui_label = "Shutter angle (degrees)";
    ui_min = 0.0;
    ui_max = 360.0;
    ui_step = 1.0;
    ui_tooltip = "In Shutter angle mode, 180 degrees exposes half a frame interval.";
>;

uniform float NativeMotionBlurShutterMs <
    ui_type = "slider";
    ui_label = "Exposure (ms)";
    ui_min = 0.0;
    ui_max = 24.0;
    ui_step = 0.25;
    ui_tooltip = "In Fixed exposure mode, duration is capped at one frame interval.";
>;

uniform int NativeMotionBlurSamples <
    ui_type = "slider";
    ui_label = "Samples";
    ui_min = 3;
    ui_max = 17;
    ui_step = 2;
    ui_tooltip = "Number of samples along the motion path, including the center pixel.";
>;

uniform float NativeMotionBlurMaxRadiusPx <
    ui_type = "slider";
    ui_label = "Maximum radius (output pixels)";
    ui_min = 0.0;
    ui_max = 32.0;
    ui_step = 1.0;
    ui_tooltip = "Maximum distance from the center to either end of the blur path.";
>;

uniform int NativeMotionBlurDisplay <
    ui_type = "combo";
    ui_label = "Display";
    ui_items = "Full image\0Split comparison\0";
    ui_tooltip = "Split comparison shows the scene on the left and the effect on the right.";
>;

uniform int NativeMotionWheelPath <
    ui_type = "combo";
    ui_label = "Wheel blur path";
    ui_items = "Circular arcs\0Straight comparison\0";
    ui_tooltip = "Circular arcs follow the wheel's projected rotation and preserve depth-tested coverage.";
>;

void NativeMotionBlurVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
    position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

// This technique exposes controls and participates in ReShade's enable/toggle
// state. Filtering is recorded by the add-on after FSR, before game HUD draws.
// It must never fall back to filtering the composited presentation image.
float4 NativeMotionBlurPS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return tex2Dlod(GameColorSampler, float4(uv, 0.0, 0.0));
}

technique NativeMotionBlur < ui_label = "GTA V Native Motion Blur (before HUD)"; >
{
    pass
    {
        VertexShader = NativeMotionBlurVS;
        PixelShader = NativeMotionBlurPS;
    }
}
