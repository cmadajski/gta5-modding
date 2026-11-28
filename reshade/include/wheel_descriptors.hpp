#pragma once

#include <cstdint>
#include <optional>

#include <d3d12.h>

namespace wheel_descriptors {

struct ConstantBufferView {
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address;
    std::uint32_t size;
    // A known destination write receives a new generation, including copies.
    std::uint64_t generation;
};

// Start on the underlying native device before the game creates descriptors.
// The caller owns initialization of MinHook and the detours' process lifetime.
bool init(ID3D12Device *native_device) noexcept;

// 'cpu_handle' is a native CBV/SRV/UAV descriptor address on this device.
// A cache miss gives no evidence about the native descriptor's contents.
std::optional<ConstantBufferView> lookup(ID3D12Device *native_device,
                                         D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle) noexcept;

// Call after the device's descriptor users have finished. Process-wide hooks
// continue forwarding calls, and a reused COM address starts without history.
void shutdown_device(ID3D12Device *native_device) noexcept;

} // namespace wheel_descriptors
