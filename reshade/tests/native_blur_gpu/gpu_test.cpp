#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fsr_probe.hpp"
#include "native_blur.hpp"

using Microsoft::WRL::ComPtr;

// The test links the renderer directly without loading ReShade.
extern "C" __declspec(dllexport) void ReShadeRegisterAddon() {}
extern "C" __declspec(dllexport) void ReShadeUnregisterAddon() {}
extern "C" __declspec(dllexport) void ReShadeLogMessage(void *, int, const char *message) { std::puts(message); }

namespace
{
constexpr unsigned output_width = 23, output_height = 11;
constexpr unsigned output_alloc_width = 28, output_alloc_height = 16;
constexpr unsigned motion_width = 14, motion_height = 7;
constexpr unsigned motion_alloc_width = 18, motion_alloc_height = 10;
constexpr unsigned static_from = 18;
constexpr auto output_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr auto read_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
using Pixel = std::array<std::uint16_t, 4>;

void check(bool valid, const char *what)
{
    if (valid)
        return;
    std::fprintf(stderr, "Failed: %s\n", what);
    std::exit(1);
}

Pixel color(unsigned x, unsigned y)
{
    if (x >= output_width || y >= output_height)
        return {0x4800, 0x4600, 0x4400, 0x3c00}; // Padding must remain unchanged.
    return {static_cast<std::uint16_t>((x % 2) ? 0x4400 : 0x0000), // HDR values include 4.0.
            static_cast<std::uint16_t>((y % 2) ? 0x4000 : 0x0000), 0x3c00,
            static_cast<std::uint16_t>((x % 3) ? 0x3800 : 0x3400)};
}

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}

D3D12_TEXTURE_COPY_LOCATION texture_location(ID3D12Resource *resource)
{
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = resource;
    location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    return location;
}

ComPtr<ID3D12Resource> make_resource(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc,
                                   D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> resource;
    check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                    nullptr, IID_PPV_ARGS(&resource))), "create resource");
    return resource;
}

struct Transfer
{
    ComPtr<ID3D12Resource> buffer;
    D3D12_TEXTURE_COPY_LOCATION location{};
};

Transfer make_transfer(ID3D12Device *device, const D3D12_RESOURCE_DESC &texture,
                       D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
{
    Transfer transfer;
    transfer.location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&texture, 0, 1, 0, &transfer.location.PlacedFootprint, nullptr, nullptr, &bytes);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    transfer.buffer = make_resource(device, desc, type, state);
    transfer.location.pResource = transfer.buffer.Get();
    return transfer;
}

struct Texture
{
    ComPtr<ID3D12Resource> resource, upload;
};

template <typename Fill>
Texture make_texture(ID3D12Device *device, ID3D12GraphicsCommandList *list, unsigned width, unsigned height,
                     DXGI_FORMAT format, Fill fill)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    if (format == output_format)
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto texture = make_resource(device, desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    const auto upload = make_transfer(device, desc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapping = nullptr;
    constexpr D3D12_RANGE no_read{};
    check(SUCCEEDED(upload.buffer->Map(0, &no_read, &mapping)), "map upload");
    for (unsigned y = 0; y < height; ++y)
        fill(static_cast<unsigned char *>(mapping) + y * upload.location.PlacedFootprint.Footprint.RowPitch, y);
    upload.buffer->Unmap(0, nullptr);
    const auto destination = texture_location(texture.Get());
    list->CopyTextureRegion(&destination, 0, 0, 0, &upload.location, nullptr);
    transition(list, texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read_state);
    return {texture, upload.buffer};
}

void execute(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *list)
{
    check(SUCCEEDED(list->Close()), "close direct list");
    ID3D12CommandList *lists[]{list};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "create fence");
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(event != nullptr, "create event");
    check(SUCCEEDED(queue->Signal(fence.Get(), 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, event)), "signal fence");
    check(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "wait for GPU");
    CloseHandle(event);
    check(SUCCEEDED(device->GetDeviceRemovedReason()), "compute and readback kept device active");
}

void verify(const native_blur::Capture &capture, const Transfer &output_readback)
{
    check(capture.width == output_width && capture.height == output_height && capture.format == output_format &&
        capture.before.size() == output_width * output_height * sizeof(Pixel) &&
        capture.after.size() == capture.before.size(), "capture dimensions, format and packed rows");

    void *mapping = nullptr;
    check(SUCCEEDED(output_readback.buffer->Map(0, nullptr, &mapping)), "map in-place result");
    unsigned reference_changed = 0, moving_changed = 0, static_changed = 0, alpha_changed = 0;
    for (unsigned y = 0; y < output_alloc_height; ++y)
    {
        const auto *row = static_cast<const std::uint8_t *>(mapping) +
            y * output_readback.location.PlacedFootprint.Footprint.RowPitch;
        for (unsigned x = 0; x < output_alloc_width; ++x)
        {
            const auto expected = color(x, y);
            const auto *copied = row + x * sizeof(Pixel);
            if (x >= output_width || y >= output_height)
            {
                check(std::memcmp(copied, expected.data(), sizeof(Pixel)) == 0, "preserve output padding");
                continue;
            }
            const auto offset = (y * output_width + x) * sizeof(Pixel);
            const auto *before = capture.before.data() + offset;
            const auto *after = capture.after.data() + offset;
            check(std::memcmp(before, expected.data(), sizeof(Pixel)) == 0, "read back exact unblurred output");
            if (std::memcmp(copied, after, sizeof(Pixel)) != 0)
            {
                Pixel observed{}, blurred{};
                std::memcpy(observed.data(), copied, sizeof(Pixel));
                std::memcpy(blurred.data(), after, sizeof(Pixel));
                std::fprintf(stderr, "At (%u, %u), output=(%04x,%04x,%04x,%04x), capture=(%04x,%04x,%04x,%04x).\n",
                    x, y, unsigned(observed[0]), unsigned(observed[1]), unsigned(observed[2]), unsigned(observed[3]),
                    unsigned(blurred[0]), unsigned(blurred[1]), unsigned(blurred[2]), unsigned(blurred[3]));
                check(false, "copy the blurred scene back into the output");
            }
            const bool changed = std::memcmp(before, after, sizeof(Pixel)) != 0;
            if (x < output_width / 2)
                reference_changed += changed;
            else if (x >= static_from)
                static_changed += changed;
            else
                moving_changed += changed;
            alpha_changed += std::memcmp(before + 6, after + 6, 2) != 0;
        }
    }
    constexpr D3D12_RANGE no_write{};
    output_readback.buffer->Unmap(0, &no_write);
    std::printf("changed pixels: reference=%u moving=%u static=%u alpha=%u\n",
        reference_changed, moving_changed, static_changed, alpha_changed);
    check(reference_changed == 0 && moving_changed > 0 && static_changed == 0 && alpha_changed == 0,
        "blur the moving region and preserve split reference, static pixels, and alpha");
}
} // namespace

int main()
{
    ComPtr<ID3D12Device> device;
    const HRESULT device_result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(device_result))
    {
        std::fprintf(stderr, "No suitable D3D12 device (HRESULT=0x%lx).\n", static_cast<unsigned long>(device_result));
        return 77;
    }

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))), "create direct queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(SUCCEEDED(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator))), "create allocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    check(SUCCEEDED(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list))), "create list");

    const auto output = make_texture(device.Get(), list.Get(), output_alloc_width, output_alloc_height,
        output_format, [](unsigned char *row, unsigned y) {
            for (unsigned x = 0; x < output_alloc_width; ++x)
            {
                const auto pixel = color(x, y);
                std::memcpy(row + x * sizeof(Pixel), pixel.data(), sizeof(Pixel));
            }
        });
    const auto motion = make_texture(device.Get(), list.Get(), motion_alloc_width, motion_alloc_height,
        DXGI_FORMAT_R32G32_FLOAT, [](unsigned char *row, unsigned y) {
            auto *values = reinterpret_cast<float *>(row);
            for (unsigned x = 0; x < motion_alloc_width; ++x)
            {
                const bool padding = x >= motion_width || y >= motion_height;
                values[x * 2] = padding || x < 11 ? 6.f : 0.f;
                values[x * 2 + 1] = padding ? 6.f : 0.f;
            }
        });

    fsr_probe::Sample sample{};
    sample.output = output.resource.Get();
    sample.output_state = output_state;
    sample.output_texture = sample.output->GetDesc();
    sample.output_valid = true;
    sample.upscale_width = output_width;
    sample.upscale_height = output_height;
    sample.render_width = motion_width;
    sample.render_height = motion_height;
    sample.scale_x = sample.scale_y = 1.f;
    sample.frame_time_ms = 16.f;
    sample.reset = true;
    transition(list.Get(), sample.output, read_state, output_state);
    native_blur::Engine engine(device.Get());
    native_blur::Controls controls{};
    controls.display = 1;
    check(!engine.record(list.Get(), sample, motion.resource.Get(), 0, controls, 1) && !engine.capture_recorded(1),
        "skip history reset without recording blur or a capture");
    sample.reset = false;
    check(engine.record(list.Get(), sample, motion.resource.Get(), 0, controls, 1), "record blur");
    check(engine.capture_recorded(1), "record readback");

    // A second pass needs the first pass's copied-back pixels on this same
    // command list. A half-pixel cap makes this consumer preserve its input.
    controls.max_radius = 0.5f;
    check(engine.record(list.Get(), sample, motion.resource.Get(), 1, controls, 2), "record same-list texture consumer");
    execute(device.Get(), queue.Get(), list.Get());

    // Read the entire destination separately to inspect its padded region.
    check(SUCCEEDED(allocator->Reset()), "reset allocator");
    check(SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "reset list");
    const auto output_readback = make_transfer(device.Get(), sample.output_texture,
        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    const auto target = texture_location(sample.output);
    transition(list.Get(), sample.output, output_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyTextureRegion(&output_readback.location, 0, 0, 0, &target, nullptr);
    transition(list.Get(), sample.output, D3D12_RESOURCE_STATE_COPY_SOURCE, output_state);
    execute(device.Get(), queue.Get(), list.Get());

    native_blur::Capture capture, consumer;
    check(engine.collect(1, capture) && !engine.collect(1, capture), "collect captured frame exactly once");
    check(engine.collect(2, consumer) && consumer.after == capture.after, "same-list texture consumer sees the blurred output");
    verify(capture, output_readback);
}
