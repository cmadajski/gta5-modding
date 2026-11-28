#include "fsr_probe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <mutex>

#include <windows.h>

#include <MinHook.h>
#include <ffx_api/dx12/ffx_api_dx12.h>
#include <ffx_api/ffx_upscale.h>

namespace fsr_probe
{
namespace
{
constexpr wchar_t fidelityfx_module[] = L"amd_fidelityfx_dx12.dll";
constexpr wchar_t game_executable[] = L"GTA5_Enhanced.exe";
constexpr std::size_t context_capacity = 16;
int module_anchor;

struct Context
{
    ffxContext handle = nullptr;
    std::uintptr_t id = 0;
    std::uint32_t flags = 0;
    FfxApiDimensions2D max_upscale_size{};
};

std::mutex context_mutex;
std::array<Context, context_capacity> contexts;
std::uintptr_t last_context_id = 0;

std::mutex install_mutex;
InstallResult install_result = InstallResult::waiting_for_module;
bool preload_attempted = false;
bool preload_in_progress = false;
std::atomic<PfnFfxCreateContext> original_create;
std::atomic<PfnFfxDestroyContext> original_destroy;
std::atomic<PfnFfxDispatch> original_dispatch;
std::atomic<Consumer> capture;
std::atomic<Consumer> post_dispatch;
std::atomic_uint64_t upscale_dispatches{0};
std::atomic_uint64_t samples{0};
std::atomic_uint64_t untracked_contexts{0};
std::atomic_uint64_t unsupported_samples{0};

ffxReturnCode_t hook_create(ffxContext *context, ffxCreateContextDescHeader *desc,
                           const ffxAllocationCallbacks *memory) noexcept
{
    Context entry;
    const ffxCreateContextDescUpscale *upscale = nullptr;
    bool dx12 = false;
    const auto *header = desc;
    for (unsigned int i = 0; header != nullptr && i < 16; ++i, header = header->pNext)
    {
        if (header->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE)
            upscale = reinterpret_cast<const ffxCreateContextDescUpscale *>(header);
        else if (header->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12)
            dx12 = true;
    }
    if (upscale != nullptr && dx12)
    {
        entry.flags = upscale->flags;
        entry.max_upscale_size = upscale->maxUpscaleSize;
    }

    const auto result = original_create.load(std::memory_order_acquire)(context, desc, memory);
    if (result != FFX_API_RETURN_OK || context == nullptr || *context == nullptr || upscale == nullptr || !dx12)
        return result;

    entry.handle = *context;
    const std::lock_guard lock(context_mutex);
    entry.id = ++last_context_id;
    Context *free_slot = nullptr;
    for (auto &slot : contexts)
    {
        if (slot.handle == entry.handle)
        {
            slot = entry;
            return result;
        }
        if (slot.handle == nullptr && free_slot == nullptr)
            free_slot = &slot;
    }
    if (free_slot != nullptr)
        *free_slot = entry;
    return result;
}

ffxReturnCode_t hook_destroy(ffxContext *context, const ffxAllocationCallbacks *memory) noexcept
{
    if (context != nullptr && *context != nullptr)
    {
        const std::lock_guard lock(context_mutex);
        for (auto &slot : contexts)
        {
            if (slot.handle == *context)
            {
                slot = {};
                break;
            }
        }
    }
    return original_destroy.load(std::memory_order_acquire)(context, memory);
}

Context find_context(ffxContext handle) noexcept
{
    const std::unique_lock lock(context_mutex, std::try_to_lock);
    if (lock.owns_lock())
    {
        for (const auto &slot : contexts)
            if (slot.handle == handle)
                return slot;
    }
    return {};
}

bool source_state(std::uint32_t ffx_state, D3D12_RESOURCE_STATES &state) noexcept
{
    switch (ffx_state)
    {
    case FFX_API_RESOURCE_STATE_COMPUTE_READ:
        state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        return true;
    case FFX_API_RESOURCE_STATE_PIXEL_READ:
        state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        return true;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        return true;
    default:
        return false;
    }
}

bool output_state(std::uint32_t ffx_state, D3D12_RESOURCE_STATES &state) noexcept
{
    switch (ffx_state)
    {
    case FFX_API_RESOURCE_STATE_COMMON:
        state = D3D12_RESOURCE_STATE_COMMON;
        return true;
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:
        state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return true;
    default:
        return source_state(ffx_state, state);
    }
}

bool single_sample_texture_2d(const D3D12_RESOURCE_DESC &texture) noexcept
{
    return texture.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && texture.DepthOrArraySize == 1 &&
           texture.SampleDesc.Count == 1 && texture.MipLevels == 1;
}

bool read_sample(const ffxDispatchDescUpscale &desc, const Context &context, Sample &sample) noexcept
{
    const auto &vectors = desc.motionVectors;
    if (desc.commandList == nullptr || vectors.resource == nullptr ||
        vectors.description.type != FFX_API_RESOURCE_TYPE_TEXTURE2D || !source_state(vectors.state, sample.source_state))
        return false;

    sample.command_list = static_cast<ID3D12GraphicsCommandList *>(desc.commandList);
    sample.motion_vectors = static_cast<ID3D12Resource *>(vectors.resource);
    sample.texture = sample.motion_vectors->GetDesc();
    if (!single_sample_texture_2d(sample.texture))
        return false;

    sample.context_id = context.id;
    sample.render_width = desc.renderSize.width;
    sample.render_height = desc.renderSize.height;
    sample.upscale_width = desc.upscaleSize.width;
    sample.upscale_height = desc.upscaleSize.height;
    if (sample.upscale_width == 0 || sample.upscale_height == 0)
    {
        sample.upscale_width = context.max_upscale_size.width;
        sample.upscale_height = context.max_upscale_size.height;
    }
    sample.scale_x = desc.motionVectorScale.x;
    sample.scale_y = desc.motionVectorScale.y;
    sample.jitter_x = desc.jitterOffset.x;
    sample.jitter_y = desc.jitterOffset.y;
    sample.frame_time_ms = desc.frameTimeDelta;
    sample.reset = desc.reset;
    sample.display_resolution_motion_vectors = (context.flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0;
    sample.jitter_cancellation = (context.flags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) != 0;
    sample.depth_inverted = (context.flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) != 0;
    sample.depth_ffx_type = desc.depth.description.type;
    sample.depth_ffx_state = desc.depth.state;
    if (desc.depth.resource != nullptr)
    {
        auto *depth = static_cast<ID3D12Resource *>(desc.depth.resource);
        sample.depth_identity = reinterpret_cast<std::uintptr_t>(depth);
        sample.depth_texture = depth->GetDesc();
        const auto &description = sample.depth_texture;
        if (sample.depth_ffx_type == FFX_API_RESOURCE_TYPE_TEXTURE2D && source_state(sample.depth_ffx_state, sample.depth_state) &&
            single_sample_texture_2d(description) && description.SampleDesc.Quality == 0 &&
            sample.render_width <= description.Width &&
            sample.render_height <= description.Height &&
            (description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0 &&
            depth_srv_format(description.Format) != DXGI_FORMAT_UNKNOWN)
        {
            sample.depth = depth;
            sample.depth_valid = true;
        }
    }

    const auto vector_width = sample.display_resolution_motion_vectors ? sample.upscale_width : sample.render_width;
    const auto vector_height = sample.display_resolution_motion_vectors ? sample.upscale_height : sample.render_height;
    return vector_width > 0 && vector_height > 0 && vector_width <= sample.texture.Width && vector_height <= sample.texture.Height &&
           std::isfinite(sample.scale_x) && std::isfinite(sample.scale_y) &&
           std::isfinite(sample.jitter_x) && std::isfinite(sample.jitter_y);
}

void read_output(const FfxApiResource &output, Sample &sample) noexcept
{
    if (output.resource == nullptr || output.resource == sample.motion_vectors ||
        output.description.type != FFX_API_RESOURCE_TYPE_TEXTURE2D)
        return;

    auto *const resource = static_cast<ID3D12Resource *>(output.resource);
    sample.output_texture = resource->GetDesc();
    if (!single_sample_texture_2d(sample.output_texture) ||
        sample.upscale_width == 0 || sample.upscale_height == 0 ||
        sample.upscale_width > sample.output_texture.Width || sample.upscale_height > sample.output_texture.Height ||
        !output_state(output.state, sample.output_state))
        return;

    sample.output = resource;
    sample.output_valid = true;
}

ffxReturnCode_t hook_dispatch(ffxContext *context, const ffxDispatchDescHeader *desc) noexcept
{
    const auto consumer = capture.load(std::memory_order_acquire);
    const auto post_consumer = post_dispatch.load(std::memory_order_acquire);
    const ffxDispatchDescUpscale *upscale = nullptr;
    Sample sample{};
    if ((consumer != nullptr || post_consumer != nullptr) && desc != nullptr && desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE)
    {
        sample.dispatch_id = upscale_dispatches.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto tracked = context != nullptr && *context != nullptr ? find_context(*context) : Context{};
        if (tracked.handle == nullptr)
            untracked_contexts.fetch_add(1, std::memory_order_relaxed);
        else if (!read_sample(*reinterpret_cast<const ffxDispatchDescUpscale *>(desc), tracked, sample))
            unsupported_samples.fetch_add(1, std::memory_order_relaxed);
        else
        {
            upscale = reinterpret_cast<const ffxDispatchDescUpscale *>(desc);
            if (consumer != nullptr)
                consumer(sample);
            samples.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const auto result = original_dispatch.load(std::memory_order_acquire)(context, desc);
    if (result == FFX_API_RETURN_OK && upscale != nullptr && post_consumer != nullptr)
    {
        read_output(upscale->output, sample);
        post_consumer(sample);
    }
    return result;
}

void *find_export(HMODULE module, const char *name) noexcept
{
    const auto address = GetProcAddress(module, name);
    HMODULE owner = nullptr;
    if (address == nullptr ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &owner) || owner != module)
        return nullptr;
    return reinterpret_cast<void *>(address);
}

template <typename Func>
bool create_hook(void *target, Func detour, std::atomic<Func> &original) noexcept
{
    void *trampoline = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void *>(detour), &trampoline) != MH_OK)
        return false;
    original.store(reinterpret_cast<Func>(trampoline), std::memory_order_release);
    return true;
}

InstallResult install_hooks(HMODULE module) noexcept
{
    void *const create = find_export(module, "ffxCreateContext");
    void *const destroy = find_export(module, "ffxDestroyContext");
    void *const dispatch = find_export(module, "ffxDispatch");
    if (create == nullptr || destroy == nullptr || dispatch == nullptr)
        return InstallResult::unsupported;

    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        return InstallResult::hook_error;

    const std::array targets{create, destroy, dispatch};
    const bool have_create = create_hook<PfnFfxCreateContext>(create, hook_create, original_create);
    const bool have_destroy = have_create && create_hook<PfnFfxDestroyContext>(destroy, hook_destroy, original_destroy);
    const bool have_dispatch = have_destroy && create_hook<PfnFfxDispatch>(dispatch, hook_dispatch, original_dispatch);

    HMODULE pinned = nullptr;
    constexpr DWORD pin_address = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
    if (!have_dispatch || !GetModuleHandleExW(pin_address, reinterpret_cast<LPCWSTR>(&module_anchor), &pinned) ||
        !GetModuleHandleExW(pin_address, reinterpret_cast<LPCWSTR>(dispatch), &pinned))
    {
        if (have_dispatch)
            MH_RemoveHook(dispatch);
        if (have_destroy)
            MH_RemoveHook(destroy);
        if (have_create)
            MH_RemoveHook(create);
        return InstallResult::hook_error;
    }

    bool queued = true;
    for (auto target : targets)
        queued = MH_QueueEnableHook(target) == MH_OK && queued;
    if (queued && MH_ApplyQueued() == MH_OK)
        return InstallResult::installed;

    // Applying a batch can fail after activating one of the hooks. Retain the
    // trampolines so a detour already on a thread's stack can still return.
    for (auto target : targets)
    {
        MH_QueueDisableHook(target);
        MH_DisableHook(target);
    }
    return InstallResult::hook_error;
}

bool get_game_provider_path(std::array<wchar_t, 32768> &path) noexcept
{
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return false;

    auto *separator = std::wcsrchr(path.data(), L'\\');
    if (separator == nullptr || _wcsicmp(separator + 1, game_executable) != 0)
        return false;

    auto *filename = separator + 1;
    if (static_cast<std::size_t>(path.data() + path.size() - filename) < std::size(fidelityfx_module))
        return false;

    std::copy_n(fidelityfx_module, std::size(fidelityfx_module), filename);
    return true;
}
} // namespace

InstallResult try_install(Consumer consumer, bool preload_game_provider, Consumer post_consumer) noexcept
{
    HMODULE module = nullptr;
    InstallResult result;
    {
        std::unique_lock lock(install_mutex);
        if (preload_in_progress)
            return InstallResult::waiting_for_module;

        if (install_result == InstallResult::waiting_for_module)
        {
            if (!GetModuleHandleExW(0, fidelityfx_module, &module) && preload_game_provider && !preload_attempted)
            {
                preload_attempted = true;
                std::array<wchar_t, 32768> path;
                if (get_game_provider_path(path))
                {
                    // The provider imports DXGI. Avoid holding the probe lock
                    // while DLL initialization can call back through ReShade.
                    preload_in_progress = true;
                    lock.unlock();
                    module = LoadLibraryExW(path.data(), nullptr,
                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                    lock.lock();
                    preload_in_progress = false;
                    if (module == nullptr)
                        return InstallResult::early_load_failed;
                }
            }
            if (module != nullptr)
                install_result = install_hooks(module);
        }
        result = install_result;
        if (result == InstallResult::installed)
        {
            capture.store(consumer, std::memory_order_release);
            post_dispatch.store(post_consumer, std::memory_order_release);
        }
    }
    if (module != nullptr)
        FreeLibrary(module);
    return result;
}

Stats get_stats() noexcept
{
    return {upscale_dispatches.load(std::memory_order_relaxed), samples.load(std::memory_order_relaxed),
            untracked_contexts.load(std::memory_order_relaxed), unsupported_samples.load(std::memory_order_relaxed)};
}

void stop_capture() noexcept
{
    capture.store(nullptr, std::memory_order_release);
    post_dispatch.store(nullptr, std::memory_order_release);
}
} // namespace fsr_probe
