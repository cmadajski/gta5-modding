#include "wheel_descriptors.hpp"

#include <MinHook.h>
#include <wrl/client.h>

#include <array>
#include <cstdlib>
#include <iostream>

namespace {

using Microsoft::WRL::ComPtr;
using wheel_descriptors::lookup;

constexpr auto type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
constexpr UINT cache_capacity = 131072;
constexpr IID device15_iid{0x76cff76f, 0x1e9b, 0x4450, {0x8c, 0xdc, 0x34, 0xf1, 0xaf, 0x78, 0x8e, 0x5b}};

void expect(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Heap {
    ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE first{};
    UINT stride;

    Heap(ID3D12Device *device, UINT count, D3D12_DESCRIPTOR_HEAP_FLAGS flags)
        : stride(device->GetDescriptorHandleIncrementSize(type))
    {
        const D3D12_DESCRIPTOR_HEAP_DESC description{type, count, flags, 0};
        expect(SUCCEEDED(device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&descriptor_heap))),
               "CreateDescriptorHeap failed");
        first = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE at(UINT index) const { return {first.ptr + SIZE_T(stride) * index}; }
};

void cbv(ID3D12Device *device, D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_GPU_VIRTUAL_ADDRESS address, UINT size = 256)
{
    const D3D12_CONSTANT_BUFFER_VIEW_DESC description{address, size};
    device->CreateConstantBufferView(&description, handle);
}

wheel_descriptors::ConstantBufferView expect_cbv(ID3D12Device *device, D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                D3D12_GPU_VIRTUAL_ADDRESS address, UINT size = 256)
{
    const auto metadata = lookup(device, handle);
    expect(metadata && metadata->gpu_address == address && metadata->size == size && metadata->generation != 0,
           "Tracked CBV payload differs");
    return *metadata;
}

D3D12_SHADER_RESOURCE_VIEW_DESC null_srv()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Texture2D.MipLevels = 1;
    return description;
}

template <typename... Parameters>
HRESULT try_create(IUnknown *device15, std::size_t slot, Parameters... parameters)
{
    using Function = HRESULT (STDMETHODCALLTYPE *)(IUnknown *, Parameters...);
    auto *const methods = *reinterpret_cast<void *const *const *>(device15);
    return reinterpret_cast<Function>(methods[slot])(device15, parameters...);
}

void test_device15(ID3D12Device *device, IUnknown *device15, const Heap &heap, D3D12_GPU_VIRTUAL_ADDRESS gpu,
                   HRESULT original_feedback_status)
{
    const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc{gpu + 256, 512};
    const auto handle = heap.at(23);
    ID3D12Resource *no_resource = nullptr;
    expect(try_create(device15, 87, &cbv_desc, handle) == S_OK, "Device15 TryCreateConstantBufferView failed");
    const auto first = expect_cbv(device, handle, gpu + 256, 512).generation;

    const auto srv = null_srv();
    expect(try_create(device15, 85, no_resource, &srv, handle) == S_OK, "Device15 TryCreateShaderResourceView failed");
    expect(!lookup(device, handle), "TryCreateShaderResourceView must invalidate an older CBV");
    expect(try_create(device15, 87, &cbv_desc, handle) == S_OK, "Device15 could not restore CBV");
    expect(expect_cbv(device, handle, gpu + 256, 512).generation > first, "Device15 CBV writes require fresh generations");

    D3D12_UNORDERED_ACCESS_VIEW_DESC description{};
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    const auto &uav = description;
    expect(try_create(device15, 86, no_resource, no_resource, &uav, handle) == S_OK, "Device15 TryCreateUnorderedAccessView failed");
    expect(!lookup(device, handle), "TryCreateUnorderedAccessView must invalidate an older CBV");

    cbv(device, handle, gpu);
    const D3D12_CONSTANT_BUFFER_VIEW_DESC *null_cbv = nullptr;
    expect(try_create(device15, 87, null_cbv, handle) == S_OK, "Device15 null CBV failed");
    expect(!lookup(device, handle), "Device15 null CBV must invalidate an older CBV");

    cbv(device, handle, gpu);
    // This input has different validation across drivers. Compare against the
    // unhooked result and require conservative metadata either way.
    const auto status = try_create(device15, 91, no_resource, no_resource, handle);
    expect(status == original_feedback_status, "Device15 feedback writer must return the original HRESULT");
    expect(!lookup(device, handle), "TryCreateSamplerFeedbackUnorderedAccessView must invalidate older metadata");
}

void test_copies(ID3D12Device *device, const Heap &source, const Heap &destination, D3D12_GPU_VIRTUAL_ADDRESS gpu)
{
    cbv(device, source.at(0), gpu);
    cbv(device, source.at(1), gpu + 256, 512);
    // Source slot 2 has a real native CBV created before tracking started.
    cbv(device, source.at(8), gpu + 512);
    cbv(device, source.at(9), gpu);
    const auto srv = null_srv();
    device->CreateShaderResourceView(nullptr, &srv, source.at(9));
    cbv(device, source.at(10), gpu + 768);
    cbv(device, destination.at(8), gpu);
    cbv(device, destination.at(10), gpu);

    const std::array dest{destination.at(0), destination.at(8), destination.at(13)};
    constexpr std::array<UINT, 3> dest_sizes{2, 3, 1};
    const std::array src{source.at(0), source.at(8)};
    constexpr std::array<UINT, 2> src_sizes{3, 3};
    device->CopyDescriptors(dest.size(), dest.data(), dest_sizes.data(), src.size(), src.data(), src_sizes.data(), type);
    const auto source_generation = expect_cbv(device, source.at(0), gpu).generation;
    const auto copy_generation = expect_cbv(device, destination.at(0), gpu).generation;
    expect(copy_generation > source_generation, "Copy must assign a new destination generation");
    expect_cbv(device, destination.at(1), gpu + 256, 512);
    expect(!lookup(device, destination.at(8)), "Copy from a pre-tracking native slot must invalidate the destination");
    expect_cbv(device, destination.at(9), gpu + 512);
    expect(!lookup(device, destination.at(10)), "Copy from a native SRV must invalidate the destination");
    expect_cbv(device, destination.at(13), gpu + 768);

    // Both optional size arrays mean one descriptor per range when null.
    const std::array dest_singles{destination.at(7), destination.at(5), destination.at(2)};
    const std::array src_singles{source.at(0), source.at(10), source.at(8)};
    device->CopyDescriptors(dest_singles.size(), dest_singles.data(), nullptr,
                            src_singles.size(), src_singles.data(), nullptr, type);
    expect_cbv(device, destination.at(7), gpu);
    expect_cbv(device, destination.at(5), gpu + 768);
    expect_cbv(device, destination.at(2), gpu + 512);
    const auto contiguous = destination.at(16);
    constexpr UINT contiguous_count = src_singles.size();
    device->CopyDescriptors(1, &contiguous, &contiguous_count, src_singles.size(), src_singles.data(), nullptr, type);
    expect_cbv(device, destination.at(16), gpu);
    expect_cbv(device, destination.at(17), gpu + 768);
    expect_cbv(device, destination.at(18), gpu + 512);

    device->CopyDescriptorsSimple(3, destination.at(18), source.at(0), type);
    expect_cbv(device, destination.at(18), gpu);
    expect_cbv(device, destination.at(19), gpu + 256, 512);
    expect(!lookup(device, destination.at(20)), "Unknown source in simple copy must remain unknown");

    device->CreateConstantBufferView(nullptr, source.at(0));
    device->CopyDescriptorsSimple(1, destination.at(18), source.at(0), type);
    expect(!lookup(device, source.at(0)), "Null CBV creation must invalidate an existing CBV");
    expect(!lookup(device, destination.at(18)), "Copy from a null CBV must invalidate known destination metadata");
    expect(expect_cbv(device, destination.at(0), gpu).generation == copy_generation,
           "Changing a source slot must leave earlier copies intact");

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(nullptr, nullptr, &uav, source.at(1));
    expect(!lookup(device, source.at(1)), "UAV overwrite must clear an existing CBV");
}

void test_eviction(ID3D12Device *device, const Heap &destination, D3D12_GPU_VIRTUAL_ADDRESS gpu)
{
    const Heap many(device, cache_capacity + 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    for (UINT i = 0; i < cache_capacity; ++i)
        cbv(device, many.at(i), gpu);
    expect_cbv(device, many.at(0), gpu); // Protect the least recently created slot.
    cbv(device, many.at(cache_capacity), gpu);
    expect_cbv(device, many.at(0), gpu);
    expect(!lookup(device, many.at(1)), "The least recently used CBV must be evicted at capacity");
    expect_cbv(device, many.at(cache_capacity), gpu);
    cbv(device, destination.at(3), gpu + 768);
    device->CopyDescriptorsSimple(1, destination.at(3), many.at(1), type);
    expect(!lookup(device, destination.at(3)), "Copy from an evicted source must clear old destination metadata");
}

} // namespace

int main()
{
    ComPtr<ID3D12Device> device;
    expect(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "D3D12CreateDevice failed");
    ComPtr<IUnknown> device15;
    const auto device15_status = device->QueryInterface(device15_iid, reinterpret_cast<void **>(device15.GetAddressOf()));
    expect(device15_status == S_OK || device15_status == E_NOINTERFACE, "Device15 interface query failed unexpectedly");
    const Heap source(device.Get(), 32, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    const Heap destination(device.Get(), 32, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);

    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 4096;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                       D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    ComPtr<ID3D12Resource> resource;
    expect(SUCCEEDED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource))),
           "CreateCommittedResource failed");
    const auto gpu = resource->GetGPUVirtualAddress();
    cbv(device.Get(), source.at(2), gpu);
    HRESULT original_feedback_status = E_NOINTERFACE;
    if (device15 != nullptr) {
        ID3D12Resource *no_resource = nullptr;
        original_feedback_status = try_create(device15.Get(), 91, no_resource, no_resource, source.at(24));
    }

    expect(!wheel_descriptors::init(device.Get()), "Tracking must refuse a device before descriptor hooks can install");
    expect(!lookup(device.Get(), source.at(2)), "Failed registration must expose no cache metadata");
    expect(MH_Initialize() == MH_OK, "MinHook initialization failed");
    expect(wheel_descriptors::init(device.Get()), "Native descriptor hooks failed to install");
    test_copies(device.Get(), source, destination, gpu);
    if (device15 != nullptr) {
        test_device15(device.Get(), device15.Get(), source, gpu, original_feedback_status);
        std::cout << "Native Device15 descriptor writes verified\n";
    }
    test_eviction(device.Get(), destination, gpu);

    cbv(device.Get(), source.at(0), gpu);
    expect_cbv(device.Get(), source.at(0), gpu);
    wheel_descriptors::shutdown_device(device.Get());
    expect(!lookup(device.Get(), source.at(0)), "Retiring a device must remove its cache");
    cbv(device.Get(), source.at(1), gpu);
    expect(!lookup(device.Get(), source.at(1)), "Detours must pass through for an unregistered device");
    expect(wheel_descriptors::init(device.Get()), "Device registration after teardown failed");
    expect(!lookup(device.Get(), source.at(0)) && !lookup(device.Get(), source.at(1)), "Reused device address kept stale data");
    wheel_descriptors::shutdown_device(device.Get());
    std::cout << "Native descriptor copy, invalidation, and eviction checks passed\n";
}
