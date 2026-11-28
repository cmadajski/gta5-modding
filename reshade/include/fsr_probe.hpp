#pragma once

#include <cstdint>

#include <d3d12.h>

namespace fsr_probe
{
// Shader-readable format for depth plane 0 of the supported D3D12 textures.
constexpr DXGI_FORMAT depth_srv_format(DXGI_FORMAT native_format) noexcept
{
    switch (native_format)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

struct Sample
{
    ID3D12GraphicsCommandList *command_list;
    ID3D12Resource *motion_vectors;
    D3D12_RESOURCE_STATES source_state;
    D3D12_RESOURCE_DESC texture;
    std::uintptr_t context_id;
    std::uint64_t dispatch_id;
    std::uint32_t render_width, render_height;
    std::uint32_t upscale_width, upscale_height;
    float scale_x, scale_y;
    float jitter_x, jitter_y;
    float frame_time_ms;
    bool reset;
    bool display_resolution_motion_vectors;
    bool jitter_cancellation;
    ID3D12Resource *output;
    D3D12_RESOURCE_STATES output_state;
    D3D12_RESOURCE_DESC output_texture;
    bool output_valid;
    ID3D12Resource *depth;
    D3D12_RESOURCE_STATES depth_state;
    // The observed layout and identity remain available even when depth is
    // unusable. Only depth_valid authorizes use of the borrowed depth pointer.
    D3D12_RESOURCE_DESC depth_texture;
    std::uintptr_t depth_identity;
    std::uint32_t depth_ffx_type, depth_ffx_state;
    bool depth_valid;
    bool depth_inverted;
};

// The pointers are borrowed for the duration of the callback. Record commands on
// command_list and restore source_state before returning. Only the post-dispatch
// callback receives output information, and may use output when output_valid is true.
// In that callback FidelityFX has restored the resources to the states reported
// in Sample. Restore those states before returning to the game.
using Consumer = void (*)(const Sample &) noexcept;

enum class InstallResult
{
    waiting_for_module,
    early_load_failed,
    installed,
    unsupported,
    hook_error
};

struct Stats
{
    std::uint64_t upscale_dispatches;
    std::uint64_t samples;
    std::uint64_t untracked_contexts;
    std::uint64_t unsupported_samples;
};

// Invoke outside the loader lock. At D3D12 device initialization, optionally
// load the provider beside GTA5_Enhanced.exe before the first FSR context.
// A successfully hooked add-on and FidelityFX DLL stay loaded for the process.
// The first callback runs before upscaling. The post callback runs only after a
// successful upscale dispatch on a tracked DX12 context with valid vectors.
// Both callbacks receive the same dispatch_id for that invocation.
InstallResult try_install(Consumer consumer, bool preload_game_provider = false,
                          Consumer post_consumer = nullptr) noexcept;
Stats get_stats() noexcept;
// An in-progress callback may still finish after capture is disabled.
void stop_capture() noexcept;
} // namespace fsr_probe
