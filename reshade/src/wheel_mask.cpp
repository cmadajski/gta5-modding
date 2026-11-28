#include "wheel_mask.hpp"

#include <limits>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace wheel_mask
{
namespace
{
constexpr std::uint64_t max_surface_bytes = 128ull * 1024 * 1024;
constexpr std::size_t max_shapes = 3;

struct Entry
{
    ID3D12Device *device;
    std::shared_ptr<Surface> surface;
    std::uint64_t bytes;
};

struct Registry
{
    std::mutex creation;
    std::mutex mutex;
    std::vector<Entry> entries;
};

Registry &registry()
{
    // The add-on can remain pinned after device teardown. Release native COM
    // objects explicitly, while no command recording can still use them.
    static auto *state = new Registry;
    return *state;
}

std::shared_ptr<Surface> find_locked(const Registry &state, ID3D12Device *device,
                                     ID3D12Resource *motion, ID3D12Resource *scene_depth)
{
    for (const auto &entry : state.entries)
        if (entry.device == device && entry.surface->motion() == motion && entry.surface->scene_depth() == scene_depth)
            return entry.surface;
    return {};
}

bool has_budget(const Registry &state, std::uint64_t bytes)
{
    if (state.entries.size() >= max_shapes || bytes > max_surface_bytes)
        return false;
    for (const auto &entry : state.entries)
    {
        if (entry.bytes > max_surface_bytes - bytes)
            return false;
        bytes += entry.bytes;
    }
    return true;
}

bool single_sample_texture(const D3D12_RESOURCE_DESC &desc)
{
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.MipLevels == 1 &&
        desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0;
}

bool matching_textures(const D3D12_RESOURCE_DESC &motion, const D3D12_RESOURCE_DESC &depth)
{
    return single_sample_texture(motion) && single_sample_texture(depth) && motion.Width != 0 && motion.Height != 0 &&
        motion.Width <= std::numeric_limits<UINT>::max() && motion.Width == depth.Width && motion.Height == depth.Height &&
        (motion.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 &&
        (depth.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
}
} // namespace

bool same_depth_layout(const D3D12_RESOURCE_DESC &source, const D3D12_RESOURCE_DESC &destination) noexcept
{
    return single_sample_texture(source) && single_sample_texture(destination) &&
        source.Format != DXGI_FORMAT_UNKNOWN && source.Format == destination.Format && source.Layout == destination.Layout &&
        source.Width != 0 && source.Height != 0 && source.Width == destination.Width && source.Height == destination.Height &&
        (source.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 &&
        (destination.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
}

void Surface::record_clear(ID3D12GraphicsCommandList *command) const noexcept
{
    if (command == nullptr)
        return;
    constexpr float clear[4]{};
    command->ClearRenderTargetView(targets_[2], clear, 0, nullptr);
    command->ClearDepthStencilView(depth_target_, D3D12_CLEAR_FLAG_DEPTH, reverse_z_ ? 0.0f : 1.0f, 0, 0, nullptr);
}

std::shared_ptr<Surface> prepare_surface(ID3D12Device *device, ID3D12Resource *motion,
                                         ID3D12Resource *scene_depth, bool reverse_z) noexcept
{
    if (device == nullptr || motion == nullptr || scene_depth == nullptr || motion == scene_depth)
        return {};
    try
    {
        auto &state = registry();
        const std::lock_guard creation(state.creation);
        {
            const std::lock_guard lock(state.mutex);
            if (auto existing = find_locked(state, device, motion, scene_depth))
                return existing->reverse_z() == reverse_z ? existing : nullptr;
        }

        const auto motion_desc = motion->GetDesc();
        const auto scene_desc = scene_depth->GetDesc();
        if (!matching_textures(motion_desc, scene_desc))
            return {};

        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{mask_format};
        constexpr auto mask_support = D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE |
            D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
            (support.Support1 & mask_support) != mask_support)
            return {};

        D3D12_RESOURCE_DESC color{};
        color.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        color.Width = motion_desc.Width;
        color.Height = motion_desc.Height;
        color.DepthOrArraySize = 1;
        color.MipLevels = 1;
        color.Format = mask_format;
        color.SampleDesc.Count = 1;
        color.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        auto depth = color;
        depth.Format = DXGI_FORMAT_R32_TYPELESS;
        depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        const auto color_bytes = device->GetResourceAllocationInfo(0, 1, &color).SizeInBytes;
        const auto depth_bytes = device->GetResourceAllocationInfo(0, 1, &depth).SizeInBytes;
        if (color_bytes > max_surface_bytes || depth_bytes > max_surface_bytes - color_bytes)
            return {};
        const auto bytes = color_bytes + depth_bytes;
        {
            const std::lock_guard lock(state.mutex);
            if (!has_budget(state, bytes))
                return {};
        }

        auto candidate = std::make_shared<Surface>();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_CLEAR_VALUE color_clear{mask_format};
        D3D12_CLEAR_VALUE depth_clear{depth_format};
        depth_clear.DepthStencil.Depth = reverse_z ? 0.0f : 1.0f;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &color,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &color_clear, IID_PPV_ARGS(&candidate->mask_))) ||
            FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, IID_PPV_ARGS(&candidate->depth_))))
            return {};

        const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3};
        const D3D12_DESCRIPTOR_HEAP_DESC dsv_heap{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
        if (FAILED(device->CreateDescriptorHeap(&rtv_heap, IID_PPV_ARGS(&candidate->rtv_heap_))) ||
            FAILED(device->CreateDescriptorHeap(&dsv_heap, IID_PPV_ARGS(&candidate->dsv_heap_))))
            return {};
        const auto base = candidate->rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr;
        const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = mask_format;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (UINT i = 0; i < candidate->targets_.size(); ++i)
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE handle{base + static_cast<SIZE_T>(i) * stride};
            device->CreateRenderTargetView(i == 2 ? candidate->mask() : nullptr, &rtv, handle);
            candidate->targets_[i] = handle;
        }
        candidate->depth_target_ = candidate->dsv_heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = depth_format;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(candidate->depth(), &dsv, candidate->depth_target_);

        candidate->width_ = static_cast<UINT>(color.Width);
        candidate->height_ = color.Height;
        candidate->reverse_z_ = reverse_z;
        candidate->motion_ = motion;
        candidate->scene_depth_ = scene_depth;

        const std::lock_guard lock(state.mutex);
        if (auto existing = find_locked(state, device, motion, scene_depth))
            return existing->reverse_z() == reverse_z ? existing : nullptr;
        if (!has_budget(state, bytes))
            return {};
        state.entries.push_back({device, candidate, bytes});
        return candidate;
    }
    catch (...) { return {}; }
}

std::shared_ptr<Surface> find_surface(ID3D12Device *device, ID3D12Resource *motion,
                                      ID3D12Resource *scene_depth) noexcept
{
    if (device == nullptr || motion == nullptr || scene_depth == nullptr)
        return {};
    try
    {
        auto &state = registry();
        const std::lock_guard lock(state.mutex);
        return find_locked(state, device, motion, scene_depth);
    }
    catch (...) { return {}; }
}

void invalidate_device(ID3D12Device *device) noexcept
{
    if (device == nullptr)
        return;
    try
    {
        auto &state = registry();
        const std::lock_guard lock(state.mutex);
        for (const auto &entry : state.entries)
            if (entry.device == device)
                entry.surface->set_initialized(false);
    }
    catch (...) {}
}

void shutdown_device(ID3D12Device *device) noexcept
{
    try
    {
        std::array<std::shared_ptr<Surface>, max_shapes> retired;
        auto &state = registry();
        const std::lock_guard lock(state.mutex);
        std::size_t count = 0;
        std::erase_if(state.entries, [&](Entry &entry) {
            if (entry.device != device)
                return false;
            retired[count++] = std::move(entry.surface);
            return true;
        });
    }
    catch (...) {}
}
} // namespace wheel_mask
