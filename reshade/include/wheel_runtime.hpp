#pragma once

#include <cstdint>
#include <optional>

#include "wheel_override.hpp"

namespace wheel_runtime
{
struct Geometry
{
    ID3D12Resource *index_buffer = nullptr;
    std::uint64_t index_offset = 0;
};

struct Frame
{
    std::uint64_t frame_id = 0;
    std::uint64_t dispatch_id = 0;
    std::uintptr_t context_id = 0;
    ID3D12Resource *motion_rt = nullptr;
    std::uint32_t render_width = 0, render_height = 0;
    std::uint32_t upscale_width = 0, upscale_height = 0;
    float jitter_x = 0, jitter_y = 0;
    bool valid = false, reset = false, jitter_cancellation = false;
    // Identity of the mask cleared by this interval's submitted post-FSR pass.
    ID3D12Resource *cleared_arc_mask = nullptr;
};

struct Coverage
{
    // Record-time history admission establishes the FSR target only.
    std::uint64_t observed = 0, record_no_history = 0, record_unsafe_seed = 0;
    std::uint64_t record_pool_full = 0, record_override_failed = 0, submitted = 0, descriptor_mismatch = 0;
    std::uint64_t missing_history = 0, missing_prior_mesh = 0;
    std::uint64_t missing_prior = 0, ambiguous_prior = 0, rejected_fit = 0, stationary = 0, claim_conflict = 0, corrected = 0;
    std::uint64_t arc_submitted = 0, arc_valid = 0, arc_invalid_projection = 0;
};

struct FrameCoverage
{
    std::uint64_t frame_id = 0, dispatch_id = 0;
    Coverage draws;
    std::uint32_t history_observations = 0;
    bool valid_fsr = false, sequential = false;
};

struct Stats
{
    std::uint64_t attempted = 0, pipeline_matches = 0, queue_admitted = 0, source_resolved = 0;
    std::uint64_t observed = 0, substituted = 0, corrected = 0, replayed = 0;
    std::uint64_t source_fallbacks = 0, wait_failures = 0, dropped = 0, unsupported_submissions = 0;
    std::uint64_t arc_recorded = 0, arc_replay_retained = 0, arc_fence_failures = 0;
    std::uint64_t upload_bytes_used = 0;
    bool blocked = false;
    Coverage coverage;
};

// The caller has initialized MinHook and keeps this add-on loaded. The queue
// must be the observed native direct presentation queue.
bool init(ID3D12Device *device, ID3D12CommandQueue *queue) noexcept;

void register_upload(ID3D12Device *device, ID3D12Resource *resource, std::uint64_t size,
                     D3D12_GPU_VIRTUAL_ADDRESS address) noexcept;
void unregister_upload(ID3D12Device *device, ID3D12Resource *resource) noexcept;

// Supplies the current complete state for a verified main-scene rigid wheel
// draw. The CPU descriptor handle is native and names the original b11 CBV.
// True means the replacement draw was recorded and the original is suppressed.
bool try_draw(ID3D12Device *device, ID3D12GraphicsCommandList *command,
              const wheel_override::GraphicsBindings &bindings, D3D12_CPU_DESCRIPTOR_HANDLE cbv,
              Geometry geometry, const wheel_override::IndexedDraw &draw, const D3D12_VIEWPORT &viewport,
              ID3D12Resource *motion_rt, const wheel_override::OutputBindings &outputs = {}) noexcept;

struct ArcPool
{
    ID3D12Resource *resource = nullptr;
    std::uint32_t stride = 0, element_count = 0;
};

// Borrowed, read-only SRV data. The device owns its lifetime.
ArcPool arc_pool(ID3D12Device *device) noexcept;

// Use a successful native Reset to replace the recording. Destroy additionally
// forgets the command-list identity, including previous queue admission.
void reset(ID3D12Device *device, ID3D12GraphicsCommandList *command) noexcept;
void destroy_command(ID3D12Device *device, ID3D12GraphicsCommandList *command) noexcept;

// Complete the interval after the actual presentation and associate observed
// submissions with its FSR sample. Missing/invalid FSR invalidates history.
void present(ID3D12Device *device, const Frame &frame) noexcept;
// Clear frame data on a lifecycle change without observing a presentation.
void invalidate_history(ID3D12Device *device) noexcept;
Stats stats(ID3D12Device *device) noexcept;
// The returned counters describe the draw's actual first submission interval.
std::optional<FrameCoverage> coverage_for_frame(ID3D12Device *device, std::uint64_t frame_id) noexcept;

// Requires proof that all GPU work using the runtime's data has completed and
// that none of its command recordings can be replayed.
void shutdown_device(ID3D12Device *device) noexcept;
} // namespace wheel_runtime
