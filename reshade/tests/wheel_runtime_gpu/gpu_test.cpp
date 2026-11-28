#include "wheel_runtime.hpp"
#include "wheel_arc_math.hpp"
#include "wheel_descriptors.hpp"
#include "wheel_mask.hpp"
#include "wheel_pipeline_hooks.hpp"

#include <MinHook.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
std::shared_ptr<wheel_pipeline_hooks::Match> fixture_match;
}

namespace wheel_pipeline_hooks
{
// The test admits one authored shader pair through the real PSO clone, without
// requiring a GTA installation or accepting a synthetic CRC in production.
std::shared_ptr<const Match> find(ID3D12PipelineState *pso) noexcept
{
    return fixture_match && fixture_match->original_pipeline.Get() == pso ? fixture_match : nullptr;
}
} // namespace wheel_pipeline_hooks

namespace
{
constexpr auto color_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr UINT width = 4;

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

ComPtr<ID3D12Resource> create_resource(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc, D3D12_HEAP_TYPE type,
                                       D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> resource;
    check_hr(
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)),
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

void transition(ID3D12GraphicsCommandList *command, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    command->ResourceBarrier(1, &barrier);
}

} // namespace

int main(int argc, char **argv)
{
    check(argc == 3, "provide vertex and pixel shader fixture paths");
    auto vs = read_shader(argv[1]);
    auto ps = read_shader(argv[2]);

    ComPtr<ID3D12Device2> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        return 77;
    check(MH_Initialize() == MH_OK, "initialize MinHook");
    check(wheel_descriptors::init(device.Get()), "hook native descriptors");
    ComPtr<ID3D12RootSignature> root;
    check_hr(device->CreateRootSignature(0, vs.data(), vs.size(), IID_PPV_ARGS(&root)),
             "create original embedded root signature");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC original{};
    original.pRootSignature = root.Get();
    original.VS = {vs.data(), vs.size()};
    original.PS = {ps.data(), ps.size()};
    for (unsigned slot = 0; slot < 3; ++slot)
        original.BlendState.RenderTarget[slot].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    original.SampleMask = UINT32_MAX;
    original.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    original.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    original.RasterizerState.DepthClipEnable = TRUE;
    original.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    original.NumRenderTargets = 3;
    std::fill_n(original.RTVFormats, original.NumRenderTargets, color_format);
    original.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    original.DepthStencilState.DepthEnable = TRUE;
    original.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    original.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    original.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> native_pipeline;
    check_hr(device->CreateGraphicsPipelineState(&original, IID_PPV_ARGS(&native_pipeline)),
             "create original pipeline");
    fixture_match = std::make_shared<wheel_pipeline_hooks::Match>();
    fixture_match->original_pipeline = native_pipeline;
    fixture_match->original_signature = root;
    check_hr(fixture_match->replacement.initialize(device.Get(), original, native_pipeline.Get(), vs),
             "cache synthetic verified pipeline");
    check(fixture_match->replacement.supports_mask(false), "create the wheel mask replay pipeline");

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    check_hr(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "create graphics queue");
    check_hr(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)), "create allocator");
    check_hr(
        device->CreateCommandList(0, queue_desc.Type, allocator.Get(), native_pipeline.Get(), IID_PPV_ARGS(&command)),
        "create direct list");

    const auto upload = create_resource(device.Get(), resource_desc(D3D12_RESOURCE_DIMENSION_BUFFER, 1280),
                                        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapping = nullptr;
    constexpr D3D12_RANGE none{};
    check_hr(upload->Map(0, &none, &mapping), "map constants and indices");
    std::memset(mapping, 0, 1280);
    auto *floats = static_cast<float *>(mapping);
    floats[0] = 0.125f;
    floats[64] = 0.25f;
    floats[128] = 0.125f;  // World first row, x component.
    floats[144] = 0.0625f; // Current first row, x component.
    floats[160] = 0.375f;  // Previous first row, x component.
    std::memcpy(floats + 192, floats + 128, 256);
    floats[224] = 0.75f;
    constexpr std::array<std::uint16_t, 3> indices{0, 1, 2};
    std::memcpy(floats + 256, indices.data(), sizeof(indices));
    upload->Unmap(0, nullptr);
    const auto gpu = upload->GetGPUVirtualAddress();
    wheel_runtime::register_upload(device.Get(), upload.Get(), 1280, gpu);
    check(wheel_runtime::init(device.Get(), queue.Get()), "hook native presentation queue");
    std::thread startup_teardown([&] { wheel_runtime::invalidate_history(device.Get()); });
    startup_teardown.join();
    const D3D12_INDEX_BUFFER_VIEW index_buffer{gpu + 1024, sizeof(indices), DXGI_FORMAT_R16_UINT};
    command->IASetIndexBuffer(&index_buffer);
    command->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const auto input = create_resource(device.Get(), resource_desc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1),
                                       D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // The wheel viewport occupies only the active portion of the native RTV.
    const auto output_desc = resource_desc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, width + 2);
    const auto output =
        create_resource(device.Get(), output_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto rtvs = create_heap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(input.Get(), nullptr, rtv);
    constexpr float input_color[]{0, 0, 0, 0.5f};
    command->ClearRenderTargetView(rtv, input_color, 0, nullptr);
    transition(command.Get(), input.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    rtv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(output.Get(), nullptr, rtv);
    constexpr float black[]{0, 0, 0, 0};
    command->ClearRenderTargetView(rtv, black, 0, nullptr);

    wheel_override::OutputBindings outputs;
    outputs.colors[0] = rtv;
    outputs.count = original.NumRenderTargets;
    const auto rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_RENDER_TARGET_VIEW_DESC null_rtv{};
    null_rtv.Format = color_format;
    null_rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (unsigned slot = 1; slot < outputs.count; ++slot)
    {
        outputs.colors[slot].ptr = rtv.ptr + slot * rtv_stride;
        device->CreateRenderTargetView(nullptr, &null_rtv, outputs.colors[slot]);
    }

    auto depth_desc = output_desc;
    depth_desc.Format = original.DSVFormat;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    const auto scene_depth = create_resource(device.Get(), depth_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const auto dsvs = create_heap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    outputs.depth = dsvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(scene_depth.Get(), nullptr, outputs.depth);
    outputs.scene_depth = scene_depth.Get();
    outputs.valid = true;
    const auto surface = wheel_mask::prepare_surface(device.Get(), output.Get(), scene_depth.Get(), false);
    check(surface != nullptr, "prepare private mask and depth");
    const auto alternate_depth = create_resource(device.Get(), depth_desc, D3D12_HEAP_TYPE_DEFAULT,
                                                  D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const auto alternate_surface = wheel_mask::prepare_surface(device.Get(), output.Get(), alternate_depth.Get(), false);
    check(alternate_surface && alternate_surface->mask() != surface->mask(), "prepare the alternate depth pair independently");
    surface->record_clear(command.Get());
    surface->set_initialized(true);
    check(!alternate_surface->initialized(), "a clear for the primary pair does not initialize its alternate");

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
    const wheel_override::GraphicsBindings original_bindings{
        root.Get(),
        native_pipeline.Get(),
        {samplers->GetGPUDescriptorHandleForHeapStart(), scene, {scene.ptr + 6 * stride}}};
    command->SetGraphicsRootSignature(root.Get());
    for (UINT i = 0; i < original_bindings.tables.size(); ++i)
        command->SetGraphicsRootDescriptorTable(i, original_bindings.tables[i]);
    const D3D12_RECT scissor{0, 0, width, 1};
    command->RSSetScissorRects(1, &scissor);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&output_desc, 0, 1, 0, &destination.PlacedFootprint, nullptr, nullptr, &bytes);
    const auto readback = create_resource(device.Get(), resource_desc(D3D12_RESOURCE_DIMENSION_BUFFER, bytes),
                                          D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    destination.pResource = readback.Get();
    D3D12_TEXTURE_COPY_LOCATION source_location{};
    source_location.pResource = output.Get();
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), 1, 0, 1};
    constexpr wheel_override::IndexedDraw draw{3, 1, 0, 0, 0};
    const wheel_runtime::Geometry geometry{upload.Get(), 1024};
    const std::array<float, 16> stock{.0625f, 0, 0, 0, 0, .0625f, 0, 0, 0, 0, .0625f, 0, 0, 0, .25f, 1};
    constexpr float angle = 0.5f;
    const auto update_source = [&](float world, float current, bool roll, float prior_clip_x = 0) {
        void *payload = nullptr;
        check_hr(upload->Map(0, &none, &payload), "write original CBV before submit");
        auto *data = static_cast<float *>(payload) + 128;
        std::fill_n(data, 48, 0);
        data[0] = world;
        std::copy(stock.begin(), stock.end(), data + 16);
        std::copy(stock.begin(), stock.end(), data + 32);
        data[16] = current;
        data[44] = prior_clip_x;
        if (roll)
        {
            data[21] = .0625f * std::cos(angle);
            data[22] = .0625f * std::sin(angle);
            data[25] = -.0625f * std::sin(angle);
            data[26] = .0625f * std::cos(angle);
        }
        upload->Unmap(0, nullptr);
    };
    ComPtr<ID3D12Fence> done;
    check_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "create independent test fence");
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(event != nullptr, "create independent test event");
    UINT64 serial = 0;
    const auto wait_gpu = [&] {
        ++serial;
        check_hr(queue->Signal(done.Get(), serial), "signal independent test fence");
        check_hr(done->SetEventOnCompletion(serial, event), "arm independent test fence");
        check(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "wait independent test fence");
        check_hr(device->GetDeviceRemovedReason(), "keep device active");
    };
    ID3D12CommandList *lists[]{command.Get()};
    check_hr(command->Close(), "close warmup command list");
    queue->ExecuteCommandLists(1, lists);
    wait_gpu();
    const auto make_recording = [&] {
        check_hr(allocator->Reset(), "reset allocator");
        check_hr(command->Reset(allocator.Get(), native_pipeline.Get()), "reset actual command list");
        wheel_runtime::reset(device.Get(), command.Get());
        command->IASetIndexBuffer(&index_buffer);
        command->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        command->OMSetRenderTargets(outputs.count, outputs.colors.data(), FALSE, &outputs.depth);
        command->ClearRenderTargetView(rtv, black, 0, nullptr);
        command->ClearDepthStencilView(outputs.depth, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        command->SetDescriptorHeaps(static_cast<UINT>(std::size(heaps)), heaps);
        command->SetGraphicsRootSignature(root.Get());
        for (UINT i = 0; i < original_bindings.tables.size(); ++i)
            command->SetGraphicsRootDescriptorTable(i, original_bindings.tables[i]);
        command->RSSetScissorRects(1, &scissor);
        command->RSSetViewports(1, &viewport);
    };
    const auto capture_and_close = [&] {
        transition(command.Get(), output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        command->CopyTextureRegion(&destination, 0, 0, 0, &source_location, nullptr);
        transition(command.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        check_hr(command->Close(), "close wheel recording");
    };
    const auto check_pixel = [&](float red, float blue, float alpha) {
        void *result = nullptr;
        const D3D12_RANGE reads{0, bytes};
        check_hr(readback->Map(0, &reads, &result), "read GPU result");
        auto *pixel = static_cast<const float *>(result);
        std::printf("Wheel GPU pixel %.6f %.6f %.6f %.6f\n", pixel[0], pixel[1], pixel[2], pixel[3]);
        check(std::abs(pixel[0] - red) < 1e-6 && std::abs(pixel[1] - .25f) < 1e-6 && std::abs(pixel[2] - blue) < 1e-6 &&
                  std::abs(pixel[3] - alpha) < 1e-6,
              "preserve expected channel values");
        readback->Unmap(0, &none);
    };
    const auto present = [&](std::uint64_t frame, std::uint64_t dispatch, ID3D12Resource *cleared_mask = nullptr) {
        wheel_runtime::present(device.Get(), {.frame_id = frame,
                                              .dispatch_id = dispatch,
                                              .context_id = 1,
                                              .motion_rt = output.Get(),
                                              .render_width = width,
                                              .render_height = 1,
                                              .upscale_width = width,
                                              .upscale_height = 1,
                                              .valid = true,
                                              .cleared_arc_mask = cleared_mask});
    };
    make_recording();
    update_source(.125f, .0625f, false);
    check(!wheel_runtime::try_draw(device.Get(), command.Get(), original_bindings, view, geometry, draw, viewport,
                                   output.Get()),
          "first observed frame emits original draw");
    command->DrawIndexedInstanced(3, 1, 0, 0, 0);
    capture_and_close();
    update_source(.125f, .0625f, true);
    queue->ExecuteCommandLists(1, lists);
    wait_gpu();
    check_pixel(.1875f, 0, .625f);
    present(10, 40);
    check(wheel_runtime::stats(device.Get()).observed == 1, "record warmup observation");
    check(!wheel_runtime::stats(device.Get()).blocked, "startup teardown does not register its thread as presenting");
    const auto warmup = wheel_runtime::coverage_for_frame(device.Get(), 10);
    check(warmup && warmup->dispatch_id == 40 && warmup->draws.observed == 1 && warmup->draws.record_no_history == 1 &&
              warmup->history_observations == 1,
          "attribute warmup to its first native submission interval");

    make_recording();
    update_source(.125f, .0625f, false, 4.f);
    check(wheel_runtime::try_draw(device.Get(), command.Get(), original_bindings, view, geometry, draw, viewport,
                                  output.Get()),
          "admit verified geometry when the draw-time prior origin is not final");
    capture_and_close();
    update_source(.25f, .125f, false);
    queue->ExecuteCommandLists(1, lists);
    wait_gpu();
    check_pixel(.25f, .0625f * std::sin(angle), .75f);
    auto stats = wheel_runtime::stats(device.Get());
    check(stats.corrected == 1 && stats.substituted == 1 && stats.upload_bytes_used == 512 && !stats.blocked,
          "correct previous field and retain private slot through replay");
    present(11, 41);
    const auto corrected = wheel_runtime::coverage_for_frame(device.Get(), 11);
    check(corrected && corrected->draws.submitted == 1 && corrected->draws.corrected == 1 && corrected->sequential,
          "attribute the rendered correction to the submitted frame");

    update_source(.375f, .1875f, false);
    queue->ExecuteCommandLists(1, lists);
    wait_gpu();
    check_pixel(.3125f, 0, .875f);
    check(wheel_runtime::stats(device.Get()).replayed == 1, "refresh stock on replay");

    ComPtr<ID3D12Fence> stall;
    check_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&stall)), "make controlled queue stall");
    check_hr(queue->Wait(stall.Get(), 1), "stall preceding GPU read");
    queue->ExecuteCommandLists(1, lists);
    std::thread release([&] {
        Sleep(60);
        check_hr(stall->Signal(1), "release controlled queue stall");
    });
    const auto started = std::chrono::steady_clock::now();
    queue->ExecuteCommandLists(1, lists);
    const auto waited = std::chrono::steady_clock::now() - started;
    release.join();
    wait_gpu();
    check(waited >= std::chrono::milliseconds(45), "wait for prior GPU read before reusing the slot on replay");
    stats = wheel_runtime::stats(device.Get());
    check(stats.replayed == 3 && stats.wait_failures == 0 && !stats.blocked, "admit safely synchronized replays");

    make_recording();
    check(wheel_runtime::stats(device.Get()).upload_bytes_used == 0,
          "release private slot after successful Reset and fence");
    update_source(.125f, .0625f, false);
    check(wheel_runtime::try_draw(device.Get(), command.Get(), original_bindings, view, geometry, draw, viewport,
                                  output.Get()),
          "record the verified geometry before the next upload is finalized");
    capture_and_close();
    update_source(.25f, .125f, false, 4.f);
    queue->ExecuteCommandLists(1, lists);
    wait_gpu();
    check_pixel(.25f, 0, .75f);
    present(12, 42);
    const auto rejected = wheel_runtime::coverage_for_frame(device.Get(), 12);
    check(rejected && rejected->draws.observed == 1 && rejected->draws.submitted == 1 &&
              rejected->draws.missing_prior == 1 && rejected->draws.corrected == 0,
          "require an actual origin match and exclude replays from frame coverage");

    make_recording();
    update_source(.125f, .0625f, true);
    if (!wheel_runtime::try_draw(device.Get(), command.Get(), original_bindings, view, geometry, draw, viewport, output.Get()))
        command->DrawIndexedInstanced(3, 1, 0, 0, 0);
    capture_and_close();
    queue->ExecuteCommandLists(1, lists);
    wait_gpu();
    present(13, 43);

    make_recording();
    update_source(.125f, .0625f, false);
    check(wheel_runtime::try_draw(device.Get(), command.Get(), original_bindings, view, geometry, draw, viewport,
                                 output.Get(), outputs),
          "record a wheel override and its depth-tested mask");
    capture_and_close();
    queue->ExecuteCommandLists(1, lists);
    wait_gpu();
    check_pixel(.1875f, .0625f * std::sin(angle), .625f);

    // The same recording is replayable, but a future arc pass still needs the
    // original data associated with its already-rendered mask pixels.
    update_source(.25f, .125f, false);
    queue->ExecuteCommandLists(1, lists);
    wait_gpu();
    check(wheel_runtime::stats(device.Get()).arc_replay_retained == 1, "freeze the arc slice during pre-clear replay");
    check_pixel(.1875f, .0625f * std::sin(angle), .625f);

    make_recording();
    check(wheel_runtime::stats(device.Get()).upload_bytes_used == 512, "keep the mask's slot after wheel fence and Reset");
    D3D12_TEXTURE_COPY_LOCATION mask_source{};
    mask_source.pResource = surface->mask();
    transition(command.Get(), surface->mask(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    command->CopyTextureRegion(&destination, 0, 0, 0, &mask_source, nullptr);
    transition(command.Get(), surface->mask(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    check_hr(command->Close(), "close mask inspection list");
    queue->ExecuteCommandLists(1, lists);
    wait_gpu();

    void *mask_pixels = nullptr;
    const D3D12_RANGE readback_range{0, bytes};
    check_hr(readback->Map(0, &readback_range, &mask_pixels), "read the rendered wheel ID");
    const auto id = std::lround(static_cast<const float *>(mask_pixels)[3] * wheel_mask::id_scale);
    readback->Unmap(0, &none);
    const auto pool = wheel_runtime::arc_pool(device.Get());
    check(pool.resource != nullptr && pool.stride == 512 && pool.element_count == 65536 && id > 0 &&
              id <= static_cast<long>(pool.element_count),
          "decode a valid structured-upload ID from the actual GPU mask");
    const auto index = static_cast<std::uint32_t>(id - 1);
    const auto offset = static_cast<SIZE_T>(index) * pool.stride;
    const D3D12_RANGE arc_range{offset, offset + pool.stride};
    void *arc_bytes = nullptr;
    check_hr(pool.resource->Map(0, &arc_range, &arc_bytes), "inspect the arc upload retained for the consumer");
    gtav::wheel_arc::WheelGpuConstants arc;
    std::memcpy(&arc, static_cast<const std::byte *>(arc_bytes) + offset, sizeof(arc));
    pool.resource->Unmap(0, &none);
    check(arc.valid == 1 && arc.frame_id == 14 && arc.canonical_id != 0 && arc.pool_index == index &&
              arc.render_width == width && arc.render_height == 1 && std::abs(arc.roll - angle) < 1e-6 &&
              arc.stock_previous[6] == 0 && std::abs(arc.corrected_previous[6] - .0625f * std::sin(angle)) < 1e-6,
          "match the mask to exact current-frame arc data after replay");
    present(14, 44);
    const auto masked = wheel_runtime::coverage_for_frame(device.Get(), 14);
    check(masked && masked->draws.arc_submitted == 1 && masked->draws.arc_valid == 1 &&
              wheel_runtime::stats(device.Get()).upload_bytes_used == 512,
          "count one valid arc and retain its slot through a present with no clear");

    make_recording();
    surface->record_clear(command.Get());
    check_hr(command->Close(), "close the simulated post-FSR mask-clear list");
    queue->ExecuteCommandLists(1, lists);
    wait_gpu();

    // A late wheel command writes a new ID after the clear but before the
    // presentation callback. The earlier clear must not retire that ID.
    make_recording();
    update_source(.125f, .0625f, false);
    check(wheel_runtime::try_draw(device.Get(), command.Get(), original_bindings, view, geometry, draw, viewport,
                                  output.Get(), outputs),
          "record a mask after the simulated FSR clear");
    capture_and_close();
    queue->ExecuteCommandLists(1, lists);
    present(15, 45, surface->mask());
    wait_gpu();
    make_recording();
    check(wheel_runtime::stats(device.Get()).upload_bytes_used == 512,
          "retain a mask written after the same presentation's clear");
    alternate_surface->record_clear(command.Get());
    check_hr(command->Close(), "close the alternate pair's mask-clear list");
    queue->ExecuteCommandLists(1, lists);
    present(16, 46, alternate_surface->mask());
    wait_gpu();
    alternate_surface->set_initialized(true);
    check(surface->initialized() && alternate_surface->initialized(), "retain readiness independently for both depth pairs");
    make_recording();
    check(wheel_runtime::stats(device.Get()).upload_bytes_used == 512,
          "retain a pending primary ID through an alternate pair's confirmed clear");
    surface->record_clear(command.Get());
    check_hr(command->Close(), "close the later mask-clear list");
    queue->ExecuteCommandLists(1, lists);
    present(17, 47, surface->mask());
    wait_gpu();
    make_recording();
    check(wheel_runtime::stats(device.Get()).upload_bytes_used == 0, "recycle the slot only after the clear fence and Reset");

    // The last presented interval contained only the mask clear. Its target
    // is known, but a verified wheel may be recorded before history arrives.
    update_source(.125f, .0625f, true);
    check(wheel_runtime::try_draw(device.Get(), command.Get(), original_bindings, view, geometry, draw, viewport,
                                  output.Get()),
          "record the known target while the previous history is empty");
    capture_and_close();
    queue->ExecuteCommandLists(1, lists);
    wait_gpu();
    check_pixel(.1875f, 0, .625f);

    // Draw a frame ahead: the preceding wheel has executed, but its exact
    // history cannot be promoted until the preceding FSR interval presents.
    make_recording();
    update_source(.25f, .125f, false);
    check(wheel_runtime::try_draw(device.Get(), command.Get(), original_bindings, view, geometry, draw, viewport,
                                  output.Get()),
          "record ahead of the previous FSR present");
    capture_and_close();
    present(18, 48);
    const auto stock_frame = wheel_runtime::coverage_for_frame(device.Get(), 18);
    check(stock_frame && stock_frame->draws.submitted == 1 && stock_frame->draws.missing_history == 1 &&
              stock_frame->draws.corrected == 0 && stock_frame->history_observations == 1,
          "preserve stock when actual submitted history is unavailable");
    queue->ExecuteCommandLists(1, lists);
    wait_gpu();
    check_pixel(.25f, .0625f * std::sin(angle), .75f);
    present(19, 49);
    const auto promoted = wheel_runtime::coverage_for_frame(device.Get(), 19);
    check(promoted && promoted->draws.corrected == 1 && promoted->draws.missing_history == 0,
          "use freshly promoted history at Execute after recording the same draw without prior history");

    wheel_runtime::present(device.Get(), {.frame_id = 20});
    make_recording();
    const auto masks_before_gap = wheel_runtime::stats(device.Get()).arc_recorded;
    update_source(.25f, .125f, false);
    check(wheel_runtime::try_draw(device.Get(), command.Get(), original_bindings, view, geometry, draw, viewport,
                                  output.Get(), outputs),
          "allow stock replacement for the known target during a single missing-FSR interval");
    check(wheel_runtime::stats(device.Get()).arc_recorded == masks_before_gap,
          "require the latest confirmed target before recording a mask, even if the old surface is still initialized");
    capture_and_close();
    queue->ExecuteCommandLists(1, lists);
    wait_gpu();
    check_pixel(.25f, 0, .75f);
    wheel_runtime::present(device.Get(), {.frame_id = 21});
    make_recording();
    check(!wheel_runtime::try_draw(device.Get(), command.Get(), original_bindings, view, geometry, draw, viewport,
                                   output.Get(), outputs),
          "expire a verified recording target after two missing-FSR intervals");
    wheel_mask::invalidate_device(device.Get());
    check(!surface->initialized() && !alternate_surface->initialized(), "invalidation affects both ready pairs");
    alternate_surface->record_clear(command.Get());
    check_hr(command->Close(), "close the alternate pair's warmup after invalidation");
    queue->ExecuteCommandLists(1, lists);
    present(22, 50, alternate_surface->mask());
    wait_gpu();
    alternate_surface->set_initialized(true);
    check(!surface->initialized() && alternate_surface->initialized(), "restore readiness only to the individually cleared pair");
    wheel_runtime::destroy_command(device.Get(), command.Get());
    wheel_runtime::shutdown_device(device.Get());
    wheel_mask::shutdown_device(device.Get());
    wheel_descriptors::shutdown_device(device.Get());
    CloseHandle(event);
}
