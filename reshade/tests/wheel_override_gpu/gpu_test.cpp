// Compile fixture.hlsl with DXC using vertex/vs_6_0 and pixel/ps_6_0.
// Link this executable with wheel_override.cpp, wheel_mask.cpp and d3d12.
#include "wheel_override.hpp"
#include "wheel_mask.hpp"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr auto color_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr UINT width = 8;

void check(bool condition, const char *operation)
{
    if (!condition)
    {
        std::fprintf(stderr, "Failed: %s\n", operation);
        std::exit(1);
    }
}

void check_hr(HRESULT result, const char *operation)
{
    if (FAILED(result))
        std::fprintf(stderr, "%s HRESULT=0x%lx\n", operation, static_cast<unsigned long>(result));
    check(SUCCEEDED(result), operation);
}

std::vector<std::byte> read_shader(const char *path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    check(file.good() && file.tellg() > 0, "open shader fixture");
    std::vector<std::byte> contents(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(contents.data()), static_cast<std::streamsize>(contents.size()));
    check(file.good(), "read shader fixture");
    return contents;
}

D3D12_RESOURCE_DESC resource_desc(D3D12_RESOURCE_DIMENSION dimension, UINT64 length)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = dimension;
    desc.Width = length;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    if (dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    else
    {
        desc.Format = color_format;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    return desc;
}

ComPtr<ID3D12Resource> create_resource(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc,
                                       D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> resource;
    check_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)),
        "create committed resource");
    return resource;
}

ComPtr<ID3D12DescriptorHeap> create_heap(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count,
                                       D3D12_DESCRIPTOR_HEAP_FLAGS flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
{
    const D3D12_DESCRIPTOR_HEAP_DESC desc{type, count, flags, 0};
    ComPtr<ID3D12DescriptorHeap> heap;
    check_hr(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)), "create descriptor heap");
    return heap;
}

void transition(ID3D12GraphicsCommandList *command, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    command->ResourceBarrier(1, &barrier);
}

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T>
struct alignas(void *) Entry
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T data;
};

struct GraphicsStream
{
    Entry<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
    Entry<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
    Entry<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Entry<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    Entry<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
    Entry<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
    Entry<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
    Entry<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Entry<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> colors;
    Entry<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depth_format;
    Entry<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
    Entry<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO, D3D12_CACHED_PIPELINE_STATE> cached;
};
} // namespace

int main(int argc, char **argv)
{
    check(argc == 3, "provide vertex and pixel shader fixture paths");
    auto vs = read_shader(argv[1]);
    auto ps = read_shader(argv[2]);

    ComPtr<ID3D12Device2> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        return 77;
    ComPtr<ID3D12RootSignature> root;
    check_hr(device->CreateRootSignature(0, vs.data(), vs.size(), IID_PPV_ARGS(&root)), "create original embedded root signature");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC original{};
    original.pRootSignature = root.Get();
    original.VS = {vs.data(), vs.size()};
    original.PS = {ps.data(), ps.size()};
    const D3D12_RENDER_TARGET_BLEND_DESC blend{
        TRUE, FALSE, D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP,
        D3D12_COLOR_WRITE_ENABLE_ALL};
    original.BlendState.IndependentBlendEnable = TRUE;
    for (auto &target : original.BlendState.RenderTarget)
        target = blend;
    original.SampleMask = UINT32_MAX;
    original.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    original.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    original.RasterizerState.DepthClipEnable = TRUE;
    original.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    original.NumRenderTargets = 5;
    for (UINT i = 0; i < original.NumRenderTargets; ++i)
        original.RTVFormats[i] = color_format;
    original.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    original.DepthStencilState.DepthEnable = TRUE;
    original.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    original.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    original.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> native_pipeline;
    check_hr(device->CreateGraphicsPipelineState(&original, IID_PPV_ARGS(&native_pipeline)), "create original pipeline");
    wheel_override::Pipeline replacement;
    check_hr(replacement.initialize(device.Get(), original, native_pipeline.Get(), vs), "clone pipeline with inline CBV");

    GraphicsStream stream{
        {.data = root.Get()}, {.data = original.VS}, {.data = original.PS}, {.data = original.BlendState},
        {.data = original.SampleMask}, {.data = original.RasterizerState}, {.data = original.DepthStencilState},
        {.data = original.PrimitiveTopologyType},
        {.data = {{color_format, color_format, color_format, color_format, color_format}, 5}},
        {.data = original.DSVFormat}, {.data = original.SampleDesc}, {.data = {}}};
    const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
    wheel_override::GraphicsShaders shaders;
    check(wheel_override::inspect_graphics_stream(stream_desc, shaders) && shaders.root_signature == root.Get() &&
        shaders.vs.pShaderBytecode == vs.data() && shaders.ps.pShaderBytecode == ps.data(), "identify shaders and root in a native stream");
    const D3D12_PIPELINE_STATE_STREAM_DESC truncated{sizeof(stream) - 1, &stream};
    check(!wheel_override::inspect_graphics_stream(truncated, shaders), "reject incomplete native stream");
    auto unknown = stream;
    unknown.cached.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MAX_VALID;
    const D3D12_PIPELINE_STATE_STREAM_DESC unknown_desc{sizeof(unknown), &unknown};
    check(!wheel_override::inspect_graphics_stream(unknown_desc, shaders), "reject unknown native stream entry");
    ComPtr<ID3D12PipelineState> stream_pipeline;
    check_hr(device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&stream_pipeline)), "create original stream pipeline");
    wheel_override::Pipeline stream_replacement;
    check_hr(stream_replacement.initialize(device.Get(), stream_desc, stream_pipeline.Get(), vs), "clone native stream with inline CBV");
    check(replacement.supports_mask(true) && stream_replacement.supports_mask(true), "create both reversed-Z mask PSOs");

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    check_hr(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "create graphics queue");
    check_hr(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)), "create allocator");
    check_hr(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), native_pipeline.Get(), IID_PPV_ARGS(&command)), "create direct list");

    const auto upload = create_resource(device.Get(), resource_desc(D3D12_RESOURCE_DIMENSION_BUFFER, 1536),
        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapping = nullptr;
    constexpr D3D12_RANGE none{};
    check_hr(upload->Map(0, &none, &mapping), "map constants and indices");
    std::memset(mapping, 0, 1536);
    auto *floats = static_cast<float *>(mapping);
    floats[0] = 0.125f;
    floats[64] = 0.25f;
    floats[128] = 0.125f; // World first row, x component.
    floats[144] = 0.0625f; // Current first row, x component.
    floats[160] = 0.375f; // Previous first row, x component.
    std::memcpy(floats + 192, floats + 128, 256);
    floats[224] = 0.75f;
    std::memcpy(floats + 320, floats + 192, 256);
    floats[336] = 0.03125f; // Farther than the first draw under reversed Z.
    constexpr std::array<std::uint16_t, 3> indices{0, 1, 2};
    std::memcpy(floats + 256, indices.data(), sizeof(indices));
    upload->Unmap(0, nullptr);
    const auto gpu = upload->GetGPUVirtualAddress();
    const D3D12_INDEX_BUFFER_VIEW index_buffer{gpu + 1024, sizeof(indices), DXGI_FORMAT_R16_UINT};
    command->IASetIndexBuffer(&index_buffer);
    command->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const auto input = create_resource(device.Get(), resource_desc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1),
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto output_desc = resource_desc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, width);
    const auto output = create_resource(device.Get(), output_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto rtvs = create_heap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 6, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(input.Get(), nullptr, rtv);
    constexpr float input_color[]{0, 0, 0, 0.5f};
    command->ClearRenderTargetView(rtv, input_color, 0, nullptr);
    transition(command.Get(), input.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    rtv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(output.Get(), nullptr, rtv);
    constexpr float black[]{0, 0, 0, 0};
    command->ClearRenderTargetView(rtv, black, 0, nullptr);
    wheel_override::OutputBindings output_bindings;
    output_bindings.count = original.NumRenderTargets;
    output_bindings.colors[0] = rtv;
    D3D12_RENDER_TARGET_VIEW_DESC null_color{};
    null_color.Format = color_format;
    null_color.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (UINT i = 1; i < output_bindings.count; ++i)
    {
        rtv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(nullptr, &null_color, rtv);
        output_bindings.colors[i] = rtv;
    }
    auto depth_desc = output_desc;
    depth_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    const auto scene_depth = create_resource(device.Get(), depth_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const auto dsvs = create_heap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    output_bindings.depth = dsvs->GetCPUDescriptorHandleForHeapStart();
    output_bindings.scene_depth = scene_depth.Get();
    output_bindings.valid = true;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(scene_depth.Get(), &dsv, output_bindings.depth);
    command->ClearDepthStencilView(output_bindings.depth, D3D12_CLEAR_FLAG_DEPTH, 0, 0, 0, nullptr);
    command->OMSetRenderTargets(output_bindings.count, output_bindings.colors.data(), FALSE, &output_bindings.depth);

    const auto surface = wheel_mask::prepare_surface(device.Get(), output.Get(), scene_depth.Get(), true);
    check(surface != nullptr, "allocate wheel coverage surface");
    surface->record_clear(command.Get());
    surface->set_initialized(true);

    const auto views = create_heap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 14);
    const auto samplers = create_heap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1);
    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&sampler, samplers->GetCPUDescriptorHandleForHeapStart());
    ID3D12DescriptorHeap *heaps[]{views.Get(), samplers.Get()};
    command->SetDescriptorHeaps(static_cast<UINT>(std::size(heaps)), heaps);

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{gpu, 256};
    auto view = views->GetCPUDescriptorHandleForHeapStart();
    const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT i = 0; i < 6; ++i, view.ptr += stride)
        device->CreateConstantBufferView(&cbv, view);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = color_format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    for (UINT i = 0; i < 5; ++i, view.ptr += stride)
        device->CreateShaderResourceView(input.Get(), &srv, view);
    cbv.BufferLocation = gpu + 256;
    for (UINT i = 0; i < 2; ++i, view.ptr += stride)
        device->CreateConstantBufferView(&cbv, view);
    cbv.BufferLocation = gpu + 512;
    device->CreateConstantBufferView(&cbv, view);

    auto scene = views->GetGPUDescriptorHandleForHeapStart();
    const wheel_override::GraphicsBindings original_bindings{root.Get(), native_pipeline.Get(), {
        samplers->GetGPUDescriptorHandleForHeapStart(), scene, {scene.ptr + 6 * stride}}};
    command->SetGraphicsRootSignature(root.Get());
    for (UINT i = 0; i < original_bindings.tables.size(); ++i)
        command->SetGraphicsRootDescriptorTable(i, original_bindings.tables[i]);
    const D3D12_RECT scissor{0, 0, width, 1};
    command->RSSetScissorRects(1, &scissor);
    const auto focus_pixel = [&](float x) {
        const D3D12_VIEWPORT viewport{x, 0, 1, 1, 0, 1};
        command->RSSetViewports(1, &viewport);
    };
    constexpr wheel_override::IndexedDraw draw{3, 1, 0, 0, 0};
    focus_pixel(0);
    command->DrawIndexedInstanced(3, 1, 0, 0, 0);
    focus_pixel(1);
    check(replacement.draw_indexed(command.Get(), original_bindings, gpu + 768, draw), "draw with cloned legacy pipeline");
    focus_pixel(2);
    command->SetPipelineState(stream_pipeline.Get());
    auto stream_bindings = original_bindings;
    stream_bindings.pipeline = stream_pipeline.Get();
    check(!stream_replacement.draw_indexed(command.Get(), original_bindings, gpu + 768, draw), "reject mismatched original pipeline");
    check(!stream_replacement.draw_indexed(command.Get(), stream_bindings, gpu + 769, draw), "reject unaligned root CBV");
    check(stream_replacement.draw_indexed(command.Get(), stream_bindings, gpu + 768, draw), "draw with cloned stream pipeline");
    focus_pixel(3);
    command->DrawIndexedInstanced(3, 1, 0, 0, 0);

    output_bindings.blend_factor = {0.234375f, 0.390625f, 0.421875f, 0.634765625f};
    command->OMSetBlendFactor(output_bindings.blend_factor.data());
    focus_pixel(4);
    command->SetPipelineState(native_pipeline.Get());
    const wheel_override::MaskDraw first_mask{output_bindings, *surface, 51347};
    check(replacement.draw_indexed(command.Get(), original_bindings, gpu + 768, draw, &first_mask), "replay original coverage");
    focus_pixel(5);
    check(replacement.draw_indexed(command.Get(), original_bindings, gpu + 768, draw, &first_mask), "replay alpha clip");
    focus_pixel(4);
    command->SetPipelineState(stream_pipeline.Get());
    const wheel_override::MaskDraw second_mask{output_bindings, *surface, 1};
    check(stream_replacement.draw_indexed(command.Get(), stream_bindings, gpu + 1280, draw, &second_mask), "replay farther coverage");
    focus_pixel(6);
    check(stream_replacement.draw_indexed(command.Get(), stream_bindings, gpu + 1280, draw, &second_mask), "replay stream mask");
    focus_pixel(7);
    command->DrawIndexedInstanced(3, 1, 0, 0, 0);

    transition(command.Get(), output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(command.Get(), surface->mask(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(command.Get(), surface->depth(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&output_desc, 0, 1, 0, &destination.PlacedFootprint, nullptr, nullptr, &bytes);
    const auto readback = create_resource(device.Get(), resource_desc(D3D12_RESOURCE_DIMENSION_BUFFER, bytes),
        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    destination.pResource = readback.Get();
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = output.Get();
    command->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    const auto mask_readback = create_resource(device.Get(), resource_desc(D3D12_RESOURCE_DIMENSION_BUFFER, bytes),
        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    destination.pResource = mask_readback.Get();
    source.pResource = surface->mask();
    command->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    const auto depth_readback = create_resource(device.Get(), resource_desc(D3D12_RESOURCE_DIMENSION_BUFFER, bytes),
        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    device->GetCopyableFootprints(&depth_desc, 0, 1, 0, &destination.PlacedFootprint, nullptr, nullptr, &bytes);
    destination.pResource = depth_readback.Get();
    source.pResource = surface->depth();
    command->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    check_hr(command->Close(), "close direct list");
    ID3D12CommandList *lists[]{command.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    check_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create fence");
    const HANDLE finished = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(finished != nullptr, "create GPU completion event");
    check_hr(queue->Signal(fence.Get(), 1), "signal fence");
    check_hr(fence->SetEventOnCompletion(1, finished), "arm completion event");
    check(WaitForSingleObject(finished, 10000) == WAIT_OBJECT_0, "wait for GPU fixture");
    CloseHandle(finished);
    check_hr(device->GetDeviceRemovedReason(), "keep D3D12 device active");

    const D3D12_RANGE reads{0, bytes};
    check_hr(readback->Map(0, &reads, &mapping), "map rendered pixels");
    constexpr std::array<float, 4> original_color{0.1875f, 0.25f, 0.375f, 0.625f};
    constexpr std::array<float, 4> fixed_color{0.1875f, 0.25f, 0.75f, 0.625f};
    const auto *pixels = static_cast<const float *>(mapping);
    for (UINT i = 0; i < width; ++i)
    {
        auto expected = i == 0 || i == 3 || i == 7 ? original_color : fixed_color;
        if (i == 6)
            expected[0] = 0.15625f;
        if (i == 5)
            expected = {};
        if (i >= 4)
            for (std::size_t component = 0; component < expected.size(); ++component)
                expected[component] *= output_bindings.blend_factor[component];
        const auto *pixel = pixels + i * 4;
        std::printf("Pixel %u: %.3f %.3f %.3f %.3f\n", i, pixel[0], pixel[1], pixel[2], pixel[3]);
        check(std::memcmp(pixel, expected.data(), sizeof(expected)) == 0, "preserve all but the overridden field and restore the next draw");
    }
    readback->Unmap(0, &none);
    check_hr(mask_readback->Map(0, &reads, &mapping), "map wheel stamps");
    const auto *stamps = static_cast<const float *>(mapping);
    for (UINT i = 0; i < width; ++i)
    {
        const float expected = static_cast<float>(i == 4 ? 51347 : (i == 6 ? 1 : 0)) / wheel_mask::id_scale;
        check(stamps[i * 4 + 3] == expected, "stamp exact ID only on surviving wheel coverage");
    }
    mask_readback->Unmap(0, &none);
    check_hr(depth_readback->Map(0, &reads, &mapping), "map wheel depth");
    const auto *depths = static_cast<const float *>(mapping);
    for (UINT i = 0; i < width; ++i)
    {
        const float expected = i == 4 ? 0.0625f : (i == 6 ? 0.03125f : 0.0f);
        check(depths[i] == expected, "match private replay depth and alpha discard");
    }
    depth_readback->Unmap(0, &none);
    check_hr(command->Reset(allocator.Get(), nullptr), "replace the completed recording");
    check_hr(command->Close(), "close replacement recording");
    wheel_mask::shutdown_device(device.Get());
}
