#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <numbers>

#include "fsr_probe.hpp"
#include "native_blur.hpp"
#include "wheel_arc_math.hpp"
#include "wheel_mask.hpp"

using Microsoft::WRL::ComPtr;
using gtav::wheel_arc::WheelGpuConstants;

extern "C" __declspec(dllexport) void ReShadeRegisterAddon() {}
extern "C" __declspec(dllexport) void ReShadeUnregisterAddon() {}
extern "C" __declspec(dllexport) void ReShadeLogMessage(void *, int, const char *message) { std::puts(message); }

namespace
{
constexpr unsigned output_width = 96, output_height = 72;
constexpr unsigned render_width = 64, render_height = 48;
constexpr unsigned motion_width = 66, motion_height = 50;
constexpr unsigned center_x = 71, center_y = 35, frame = 5, wheel = 11;
constexpr auto motion_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
using Pixel = std::array<float, 4>;

void check(bool passed, const char *message)
{
    if (passed)
        return;
    std::fprintf(stderr, "Failed: %s\n", message);
    std::exit(1);
}

ComPtr<ID3D12Resource> resource(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc,
                                D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                                const D3D12_CLEAR_VALUE *clear = nullptr)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> result;
    check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        state, clear, IID_PPV_ARGS(&result))), "create resource");
    return result;
}

D3D12_RESOURCE_DESC texture(unsigned width, unsigned height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    return desc;
}

ComPtr<ID3D12Resource> upload_buffer(ID3D12Device *device, UINT64 size)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return resource(device, desc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
}

struct ImageUpload
{
    ComPtr<ID3D12Resource> buffer;
    D3D12_TEXTURE_COPY_LOCATION source{};
};

template <typename Color>
ImageUpload upload_image(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc, Color color)
{
    ImageUpload upload;
    UINT64 bytes = 0;
    upload.source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    auto &footprint = upload.source.PlacedFootprint;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    upload.buffer = upload_buffer(device, bytes);
    upload.source.pResource = upload.buffer.Get();
    constexpr D3D12_RANGE no_read{};
    void *mapping = nullptr;
    check(SUCCEEDED(upload.buffer->Map(0, &no_read, &mapping)), "map image upload");
    for (unsigned y = 0; y < desc.Height; ++y)
        for (unsigned x = 0; x < desc.Width; ++x)
        {
            const auto value = color(x, y);
            auto *pixel = static_cast<unsigned char *>(mapping) + y * footprint.Footprint.RowPitch + x * sizeof(value);
            std::memcpy(pixel, &value, sizeof(value));
        }
    upload.buffer->Unmap(0, nullptr);
    return upload;
}

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *image,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {image, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}

void copy_image(ID3D12GraphicsCommandList *list, ID3D12Resource *output, const ImageUpload &upload)
{
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = output;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&destination, 0, 0, 0, &upload.source, nullptr);
}

Pixel color(unsigned x, unsigned y)
{
    // A quarter-turn each way visits red and green. A shorter shutter visits
    // blue. A linear horizontal pass sees the separate white swatch.
    if (x >= 49 && x <= 51 && y >= 13 && y <= 15)
        return {1, 0, 0, 0.75f};
    if (x >= 49 && x <= 51 && y >= 55 && y <= 57)
        return {0, 1, 0, 0.75f};
    if (x >= 67 && x <= 69 && y >= 22 && y <= 24)
        return {0, 0, 1, 0.75f};
    if (x >= 82 && x <= 84 && y == center_y)
        return {1, 1, 1, 0.75f};
    return {0, 0, 0, 0.5f};
}

Pixel captured_center(const native_blur::Capture &capture)
{
    check(capture.width == output_width && capture.height == output_height &&
        capture.format == DXGI_FORMAT_R32G32B32A32_FLOAT &&
        capture.after.size() == output_width * output_height * sizeof(Pixel), "capture full float32 frame");
    const auto offset = (center_y * output_width + center_x) * sizeof(Pixel);
    Pixel pixel{};
    std::memcpy(pixel.data(), capture.after.data() + offset, sizeof(pixel));
    return pixel;
}

void expect(const native_blur::Capture &capture, Pixel expected, const char *message)
{
    const auto pixel = captured_center(capture);
    for (unsigned i = 0; i < pixel.size(); ++i)
        if (std::abs(pixel[i] - expected[i]) >= 0.0005f)
        {
            std::fprintf(stderr, "Center =(%.6f, %.6f, %.6f, %.6f), expected (%.6f, %.6f, %.6f, %.6f)\n",
                pixel[0], pixel[1], pixel[2], pixel[3], expected[0], expected[1], expected[2], expected[3]);
            check(false, message);
        }
}

void clear_id(ID3D12GraphicsCommandList *list, const wheel_mask::Surface &surface, unsigned index, D3D12_RECT rect)
{
    const float rgba[]{0, 0, 0, float(index + 1u) / float(wheel_mask::id_scale)};
    list->ClearRenderTargetView(surface.targets()[2], rgba, 1, &rect);
}
void run_fixture(ID3D12Device *device, DXGI_FORMAT depth_format, DXGI_FORMAT dsv_format)
{
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    check(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))), "create direct queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(SUCCEEDED(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator))), "create allocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    check(SUCCEEDED(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list))), "create list");
    ComPtr<ID3D12Fence> fence;
    check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "create fence");
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(event != nullptr, "create fence event");

    const auto output_desc = texture(output_width, output_height, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto native_motion_desc = texture(motion_width, motion_height, DXGI_FORMAT_R32G32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const auto motion_desc = texture(render_width, render_height, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    const auto depth_desc = texture(motion_width, motion_height, depth_format,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_footprint{};
    UINT64 depth_row_bytes = 0;
    device->GetCopyableFootprints(&depth_desc, 0, 1, 0, &depth_footprint, nullptr, &depth_row_bytes, nullptr);
    const auto depth_pixel_bytes = depth_row_bytes / motion_width;
    check(depth_pixel_bytes == 4 || depth_pixel_bytes == 8, "float depth plane layout");
    const auto output = resource(device, output_desc, D3D12_HEAP_TYPE_DEFAULT, output_state);
    const auto native_motion = resource(device, native_motion_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto motion = resource(device, motion_desc, D3D12_HEAP_TYPE_DEFAULT, motion_state);
    const auto make_depth = [&] { return resource(device, depth_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE); };
    const auto source_depth = make_depth();
    const bool alternate_depths = depth_format == DXGI_FORMAT_R32G8X24_TYPELESS;
    const std::array final_depths{alternate_depths ? make_depth() : source_depth,
        alternate_depths ? make_depth() : source_depth};
    ComPtr<ID3D12DescriptorHeap> scene_dsv_heap;
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2};
    check(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&scene_dsv_heap))), "create scene DSV heap");
    const auto dsv_start = scene_dsv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    const auto dsv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> scene_dsv{{{dsv_start}, {dsv_start + dsv_stride}}};
    D3D12_DEPTH_STENCIL_VIEW_DESC depth_view{};
    depth_view.Format = dsv_format;
    depth_view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    for (unsigned i = 0; i < final_depths.size(); ++i)
        device->CreateDepthStencilView(final_depths[i].Get(), &depth_view, scene_dsv[i]);
    const auto surface = wheel_mask::prepare_surface(device, native_motion.Get(), source_depth.Get(), true);
    check(surface != nullptr, "create arc auxiliary textures");
    surface->set_initialized(true);

    const auto scene_upload = upload_image(device, output_desc, color);
    const auto native_vector = [](unsigned x, unsigned y) {
        // Pure half-turn around raster center (34.5, 24.5).
        return std::array{-2.0f * (float(x) - 34.0f), -2.0f * (float(y) - 24.0f)};
    };
    const auto motion_upload = upload_image(device, motion_desc, native_vector);
    const auto foreign_motion_upload = upload_image(device, motion_desc, [&](unsigned x, unsigned y) {
        auto motion = native_vector(x, y);
        if (x >= 33 && x < 36 && y >= 37 && y < 40)
            motion[0] += 0.5f;
        return motion;
    });
    const auto stock_motion_upload = upload_image(device, motion_desc, [](unsigned, unsigned) {
        return std::array{16.0f, 0.0f};
    });

    auto first = WheelGpuConstants{};
    first.current = {0, 0, 1, 0, 28.0f / render_width, 0, 0, 0, 0, -28.0f / render_height, 0, 0,
        2.0f * 34.5f / render_width - 1.0f, 1.0f - 2.0f * 24.5f / render_height, 0.5f, 1};
    first.stock_previous = first.current;
    first.corrected_previous = first.current;
    for (unsigned cell = 4; cell < 12; ++cell)
        first.corrected_previous[cell] = -first.corrected_previous[cell];
    first.frame_id = frame;
    first.canonical_id = wheel;
    first.render_width = render_width;
    first.render_height = render_height;
    check(gtav::wheel_arc::prepare_constants(first, std::numbers::pi_v<double>), "prepare circle GPU parameters");
    auto alias = first;
    alias.pool_index = 1;
    auto other_wheel = first;
    other_wheel.pool_index = 2;
    other_wheel.canonical_id = wheel + 1;
    const std::array parameters{first, alias, other_wheel};
    const auto constants = upload_buffer(device, sizeof(parameters));
    void *mapping = nullptr;
    constexpr D3D12_RANGE no_read{};
    check(SUCCEEDED(constants->Map(0, &no_read, &mapping)), "map wheel data");
    std::memcpy(mapping, parameters.data(), sizeof(parameters));
    constants->Unmap(0, nullptr);

    fsr_probe::Sample sample{};
    sample.output = output.Get();
    sample.output_state = output_state;
    sample.output_texture = output_desc;
    sample.output_valid = true;
    sample.motion_vectors = native_motion.Get();
    sample.texture = native_motion_desc;
    sample.depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    sample.depth_texture = depth_desc;
    sample.depth_valid = sample.depth_inverted = true;
    sample.upscale_width = output_width;
    sample.upscale_height = output_height;
    sample.render_width = render_width;
    sample.render_height = render_height;
    sample.scale_x = sample.scale_y = 1.0f;
    sample.jitter_x = sample.jitter_y = 0.4f;
    sample.frame_time_ms = 16.0f;

    native_blur::Engine engine(device);
    UINT64 submitted = 0;
    const auto run = [&](float radius, bool occluded, bool foreign, bool use_arcs, unsigned arc_frame,
                         const ImageUpload &motion_source) {
        const unsigned depth_index = submitted % final_depths.size();
        sample.depth = final_depths[depth_index].Get();
        transition(list.Get(), motion.Get(), motion_state, D3D12_RESOURCE_STATE_COPY_DEST);
        copy_image(list.Get(), motion.Get(), motion_source);
        transition(list.Get(), motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, motion_state);
        transition(list.Get(), output.Get(), output_state, D3D12_RESOURCE_STATE_COPY_DEST);
        copy_image(list.Get(), output.Get(), scene_upload);
        transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, output_state);
        surface->record_clear(list.Get());
        // The center's unjittered integer render texel is (47, 23). This
        // one-texel mask makes the test depend on correct positive jitter.
        clear_id(list.Get(), *surface, 0, {48, 24, 49, 25});
        clear_id(list.Get(), *surface, foreign ? 2 : 1, {33, 9, 36, 12});
        clear_id(list.Get(), *surface, 0, {33, 37, 36, 40});
        clear_id(list.Get(), *surface, 1, {44, 14, 48, 18});
        list->ClearDepthStencilView(surface->depth_target(), D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, nullptr);
        list->ClearDepthStencilView(scene_dsv[depth_index], D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, nullptr);
        if (occluded)
        {
            constexpr D3D12_RECT bottom{33, 37, 36, 40};
            list->ClearDepthStencilView(scene_dsv[depth_index], D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, 1, &bottom);
        }

        native_blur::Controls controls;
        controls.shutter_angle = 360.f;
        controls.samples = 3;
        controls.max_radius = radius;
        controls.wheel_arcs = use_arcs;
        const native_blur::WheelArcInputs arcs{surface.get(), constants.Get(), unsigned(parameters.size()), arc_frame};
        check(engine.record(list.Get(), sample, motion.Get(), 0, controls, 1, &arcs), "record native pre-HUD blur");
        check(SUCCEEDED(list->Close()), "close command list");
        ID3D12CommandList *commands[]{list.Get()};
        queue->ExecuteCommandLists(1, commands);
        ++submitted;
        check(SUCCEEDED(queue->Signal(fence.Get(), submitted)) &&
            SUCCEEDED(fence->SetEventOnCompletion(submitted, event)) &&
            WaitForSingleObject(event, 10000) == WAIT_OBJECT_0 && SUCCEEDED(device->GetDeviceRemovedReason()),
            "execute blur and readback");
        check(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "replace completed recording");
        native_blur::Capture capture;
        check(engine.collect(1, capture), "collect scene");
        if (use_arcs)
        {
            check(capture.wheel_width == motion_width && capture.wheel_height == motion_height &&
                capture.wheel_parameter_stride == sizeof(WheelGpuConstants), "capture padded arc layout");
            check(capture.wheel_mask.size() == motion_width * motion_height * sizeof(Pixel) &&
                capture.wheel_depth.size() == motion_width * motion_height * sizeof(float) &&
                capture.scene_depth.size() == depth_row_bytes * motion_height &&
                capture.wheel_parameters.size() == sizeof(parameters), "capture all arc planes and parameter bytes");
            check(capture.scene_depth_format == depth_footprint.Footprint.Format &&
                capture.scene_depth_row_bytes == depth_row_bytes, "publish the native plane-zero readback layout");
            Pixel captured_mask;
            float captured_depth, captured_scene_depth, captured_scene_bottom;
            const auto center = 24 * motion_width + 48;
            const auto bottom = 38 * motion_width + 34;
            std::memcpy(captured_mask.data(), capture.wheel_mask.data() + center * sizeof(Pixel), sizeof(Pixel));
            std::memcpy(&captured_depth, capture.wheel_depth.data() + center * sizeof(float), sizeof(float));
            std::memcpy(&captured_scene_depth, capture.scene_depth.data() + center * depth_pixel_bytes, sizeof(float));
            std::memcpy(&captured_scene_bottom, capture.scene_depth.data() + bottom * depth_pixel_bytes, sizeof(float));
            check(captured_mask[3] == 1.0f / wheel_mask::id_scale && captured_depth == 0.5f,
                "snapshot mask and depth before their post-FSR clear");
            check(captured_scene_depth == 0.5f && captured_scene_bottom == (occluded ? 0.75f : 0.5f),
                "snapshot float final scene depth from plane zero");
            check(std::memcmp(capture.wheel_parameters.data(), parameters.data(), sizeof(parameters)) == 0,
                "snapshot byte-exact submitted arc constants");
        }
        return capture;
    };

    expect(run(32, false, false, true, frame, motion_upload), {0.25f, 0.25f, 0, 0.5f},
        "gather both quarter-circle endpoints, including a second draw of the same wheel");
    expect(run(32, true, false, true, frame, motion_upload), {1.0f / 3.0f, 0, 0, 0.5f},
        "reject the endpoint hidden behind final scene depth");
    expect(run(32, false, true, true, frame, motion_upload), {0, 1.0f / 3.0f, 0, 0.5f},
        "reject the endpoint owned by a different wheel");
    expect(run(12, false, false, true, frame, motion_upload), {0, 0, 1.0f / 3.0f, 0.5f},
        "shorten shutter to the blue curved sample within the pixel cap");
    expect(run(32, false, false, true, frame, foreign_motion_upload), {1.0f / 3.0f, 0, 0, 0.5f},
        "reject a tap whose native motion disagrees despite matching wheel ID and depth");
    const auto stock = run(32, false, false, false, frame, stock_motion_upload);
    const auto stale = run(32, false, false, true, frame + 1, stock_motion_upload);
    const auto foreign_center = run(32, false, false, true, frame, stock_motion_upload);
    check(stock.after == stale.after && stock.after == foreign_center.after && captured_center(stock)[0] > 0.1f,
        "stale history or mismatched center motion falls back to the same nonzero linear blur");
    check(engine.stats().arc_frames == 7 && engine.stats().arc_clears == 8, "arc and clear recording counters");
    check(SUCCEEDED(list->Close()), "close replacement command list");
    CloseHandle(event);
    wheel_mask::shutdown_device(device);
    std::printf("Wheel arc gather, ownership, depth, jitter, cap and stale history passed (depth resource=%u, copy plane=%u).\n",
        unsigned(depth_format), unsigned(depth_footprint.Footprint.Format));
}
} // namespace

int main()
{
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        return 77;
    run_fixture(device.Get(), DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT);
    run_fixture(device.Get(), DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
}
