#pragma once

#include <array>
#include <cstdint>
#include <atomic>
#include <memory>

#include <d3d12.h>
#include <wrl/client.h>

namespace wheel_mask
{
inline constexpr std::uint32_t id_scale = 65536;
inline constexpr DXGI_FORMAT mask_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
inline constexpr DXGI_FORMAT depth_format = DXGI_FORMAT_D32_FLOAT;

// The copy used for visibility must preserve native depth values and geometry.
bool same_depth_layout(const D3D12_RESOURCE_DESC &source, const D3D12_RESOURCE_DESC &destination) noexcept;

class Surface
{
public:
    ID3D12Resource *mask() const noexcept { return mask_.Get(); }
    ID3D12Resource *depth() const noexcept { return depth_.Get(); }
    // Native depth attachment used to rasterize wheel geometry. FSR can read
    // a same-layout copy of this attachment through a different resource.
    ID3D12Resource *scene_depth() const noexcept { return scene_depth_.Get(); }
    ID3D12Resource *motion() const noexcept { return motion_.Get(); }
    UINT width() const noexcept { return width_; }
    UINT height() const noexcept { return height_; }
    bool reverse_z() const noexcept { return reverse_z_; }
    bool initialized() const noexcept { return initialized_.load(std::memory_order_acquire); }
    void set_initialized(bool value) noexcept { initialized_.store(value, std::memory_order_release); }

    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3> &targets() const noexcept { return targets_; }
    D3D12_CPU_DESCRIPTOR_HANDLE depth_target() const noexcept { return depth_target_; }

    // The caller owns queue ordering and both resources must be in their
    // writable D3D12 states on this command list.
    void record_clear(ID3D12GraphicsCommandList *command) const noexcept;

private:
    friend std::shared_ptr<Surface> prepare_surface(ID3D12Device *, ID3D12Resource *, ID3D12Resource *, bool) noexcept;

    Microsoft::WRL::ComPtr<ID3D12Resource> motion_, scene_depth_, mask_, depth_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_, dsv_heap_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3> targets_{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth_target_{};
    UINT width_ = 0, height_ = 0;
    bool reverse_z_ = true;
    std::atomic_bool initialized_{false};
};

// The registry retains both source identities and the private textures. All
// found surfaces stay alive until the device is drained and shut down.
std::shared_ptr<Surface> prepare_surface(ID3D12Device *device, ID3D12Resource *motion,
                                         ID3D12Resource *scene_depth, bool reverse_z) noexcept;
std::shared_ptr<Surface> find_surface(ID3D12Device *device, ID3D12Resource *motion,
                                      ID3D12Resource *scene_depth) noexcept;
// Keep all resource pairs alive while requiring a new clear on each one.
void invalidate_device(ID3D12Device *device) noexcept;
void shutdown_device(ID3D12Device *device) noexcept;
} // namespace wheel_mask
