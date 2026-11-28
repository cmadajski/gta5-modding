#define WHEEL_LAYOUT "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "DescriptorTable(Sampler(s0, numDescriptors=unbounded, space=2, flags=DESCRIPTORS_VOLATILE, offset=0))," \
    "DescriptorTable(CBV(b6), CBV(b5), CBV(b9), CBV(b2), CBV(b7), CBV(b3))," \
    "DescriptorTable(SRV(t17, space=1), SRV(t0, space=1), SRV(t16, space=1), SRV(t15, space=1)," \
    "SRV(t18, space=1), CBV(b12, space=1), CBV(b10, space=1), CBV(b11, space=1))"

cbuffer Scene : register(b6) { float scene_value; }
cbuffer Material : register(b12, space1) { float material_value; }
cbuffer Wheel : register(b11, space1) { float4 world[4], current[4], previous[4]; }
Texture2D<float4> Color : register(t17, space1);
SamplerState Point : register(s0, space2);

struct Vertex
{
    float4 position : SV_Position;
    nointerpolation float velocity : TEXCOORD0;
    nointerpolation float current_value : TEXCOORD1;
};

[RootSignature(WHEEL_LAYOUT)]
Vertex vertex(uint index : SV_VertexID)
{
    Vertex vertex;
    float2 xy = float2((index & 2) ? 3 : -1, (index & 1) ? 3 : -1);
    vertex.position = float4(xy, current[0].x, 1);
    vertex.velocity = previous[1].z;
    vertex.current_value = current[0].x;
    return vertex;
}

struct Pixel
{
    float4 color : SV_Target0;
    float4 mask_material : SV_Target2;
};

[RootSignature(WHEEL_LAYOUT)]
Pixel pixel(Vertex vertex)
{
    Pixel pixel;
    pixel.color = float4(scene_value + vertex.current_value, material_value, vertex.velocity,
        Color.SampleLevel(Point, float2(0.5, 0.5), 0).a + world[0].x);
    pixel.mask_material = float4(0, 0, 0, 1);
    return pixel;
}
