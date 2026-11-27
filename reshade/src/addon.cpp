#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <reshade.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "fsr_probe.hpp"
#include "native_motion_math.hpp"
#include "native_blur.hpp"

using Microsoft::WRL::ComPtr;
namespace api = reshade::api;
namespace math = gtav::native_motion;

extern "C" __declspec(dllexport) const char *NAME = "GTA V Native Motion Vectors";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "FSR 3.1 native motion-vector diagnostic for GTA V Enhanced / Proton. No optical flow.";

namespace
{
constexpr char effect_file[] = "NativeMotionVectors.fx";
constexpr char technique_name[] = "NativeMotionVisualization";
constexpr char blur_effect_file[] = "NativeMotionBlur.fx";
constexpr char blur_technique_name[] = "NativeMotionBlur";
constexpr std::array effect_files{effect_file, blur_effect_file};
constexpr char semantic[] = "GTAV_NATIVE_MOTION";
// Standard D3D12 private data bridges a game's proxy command list to ReShade's
// original command list, without depending on private ReShade interface IDs.
constexpr GUID command_tag = {0xb835f283, 0x09b2, 0x4835, {0xa4, 0x73, 0xa8, 0x8d, 0xf0, 0x68, 0x7d, 0xb9}};
constexpr std::size_t slot_count = 32;
constexpr std::uint64_t memory_budget = 256ull * 1024 * 1024;
constexpr std::uint32_t verification_frame_count = 120;

enum class Phase { free, recorded, submitted, retiring, quarantined };
struct Slot
{
    api::resource texture{};
    api::resource_view view{};
    std::uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t bytes = 0, command = 0, sequence = 0, retirement = 0;
    Phase phase = Phase::free;
    bool recording_replaced = false;
    fsr_probe::Sample metadata{};
    math::float2 pixel_scale{};
};

struct DeviceState
{
    api::device *device = nullptr;
    api::effect_runtime *runtime = nullptr;
    api::command_queue *queue = nullptr;
    ComPtr<ID3D12Fence> fence;
    std::array<Slot, slot_count> slots;
    Slot display;
    bool binding_dirty = true;
    DWORD present_thread = 0;
    std::uint64_t fence_value = 0, sequence = 0, displayed = 0, memory = 0;
    std::uint64_t rejected = 0, dropped = 0, allocation_failures = 0, frames = 0;
    bool queue_mismatch = false;
    bool last_valid = false;
    bool warned_format = false;
    bool warned_unobserved = false;
    bool technique_rendered = false;
    bool verification_frame_rendered = false;
    std::uint32_t verification_frames = 0;
    bool last_blur_valid = false, blur_technique_rendered = false, blur_frame_rendered = false;
    std::uint64_t blur_frames = 0, capture_pair_sequence = 0;
    unsigned capture_pair_index = 0;
    bool capture_pair_pending = false;
    std::unique_ptr<native_blur::Engine> prehud;
    native_blur::Controls blur_controls{};
    bool blur_enabled = false;
    struct SceneCapture
    {
        std::uint64_t sequence = 0, retirement = 0;
        unsigned submissions = 0;
        bool final_saved = false, collected = false, recording_replaced = false;
    };
    std::array<SceneCapture, 4> scene_captures;
};

std::recursive_mutex state_mutex;
std::unordered_map<api::device *, std::unique_ptr<DeviceState>> devices;
struct CommandState
{
    api::device *device;
    api::command_list *list;
    bool reset_requested = false;
    api::command_queue *observed_queue = nullptr;
    bool mixed_queues = false;
};
std::unordered_map<std::uint64_t, CommandState> commands;
std::atomic_uint64_t next_command{1};
std::atomic_bool stopping{false};
std::atomic<fsr_probe::InstallResult> last_install{fsr_probe::InstallResult::waiting_for_module};
using ResetCommandList = HRESULT (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *);
std::atomic<ResetCommandList> original_reset{nullptr};
std::atomic<void *> installed_reset_target{nullptr};
std::once_flag reset_hook_install;

void log(const char *message) { reshade::log::message(reshade::log::level::info, message); }
void warn(const char *message) { reshade::log::message(reshade::log::level::warning, message); }
bool blur_verification_requested();

std::uint64_t command_id(ID3D12GraphicsCommandList *command)
{
    UINT size = sizeof(std::uint64_t);
    std::uint64_t id = 0;
    if (FAILED(command->GetPrivateData(command_tag, &size, &id)) || size != sizeof(id))
        return 0;
    return id;
}

void replace_recording(DeviceState &state, std::uint64_t command)
{
    for (auto &previous : state.slots)
    {
        if (previous.command != command)
            continue;
        previous.recording_replaced = true;
        for (auto &capture : state.scene_captures)
            if (capture.sequence == previous.sequence)
                capture.recording_replaced = true;
        if (previous.phase != Phase::recorded)
            continue;
        // An unobserved/native queue may still hold work from this recording.
        previous.phase = Phase::quarantined;
        if (!state.warned_unobserved)
        {
            warn("[NativeMotion] A captured recording was replaced without an observed submission. Its allocation is quarantined; save the log if capture stalls.");
            state.warned_unobserved = true;
        }
    }
}

HRESULT STDMETHODCALLTYPE track_native_reset(ID3D12GraphicsCommandList *command, ID3D12CommandAllocator *allocator,
                                              ID3D12PipelineState *initial_state) noexcept
{
    const HRESULT result = original_reset.load(std::memory_order_acquire)(command, allocator, initial_state);
    if (FAILED(result) || stopping.load(std::memory_order_relaxed))
        return result;

    try
    {
        const auto id = command_id(command);
        if (id == 0)
            return result;
        const std::lock_guard lock(state_mutex);
        const auto found = commands.find(id);
        if (found != commands.end())
        {
            if (const auto device = devices.find(found->second.device); device != devices.end())
                replace_recording(*device->second, id);
            found->second.reset_requested = false;
        }
    }
    catch (...) { /* Preserve the command-list result if tracking fails. */ }
    return result;
}

void *native_reset_target(ID3D12GraphicsCommandList *command) noexcept
{
    // The public COM ABI puts Reset after IUnknown (3), ID3D12Object (4),
    // ID3D12DeviceChild::GetDevice, ID3D12CommandList::GetType, and Close.
    constexpr std::size_t reset_index = 10;
    return (*reinterpret_cast<void *const *const *>(command))[reset_index];
}

void install_native_reset_hook(api::command_list *command)
{
    // The FidelityFX hook has already initialized MinHook and pinned the add-on.
    std::call_once(reset_hook_install, [command] {
        auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(command->get_native());
        void *const target = native_reset_target(native);
        constexpr DWORD pin_address = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
        HMODULE module = nullptr;
        void *trampoline = nullptr;
        if (GetModuleHandleExW(pin_address, reinterpret_cast<LPCWSTR>(target), &module) &&
            MH_CreateHook(target, reinterpret_cast<void *>(track_native_reset), &trampoline) == MH_OK)
        {
            original_reset.store(reinterpret_cast<ResetCommandList>(trampoline), std::memory_order_release);
            if (MH_EnableHook(target) == MH_OK)
            {
                installed_reset_target.store(target, std::memory_order_release);
                log("[NativeMotion] Tracking successful native D3D12 command-list resets.");
                return;
            }
            MH_RemoveHook(target);
        }
        warn("[NativeMotion] Could not observe successful native D3D12 command-list resets. Reusing captures only after a new FSR recording.");
    });
}

// Explicitly supported signed, directly sampleable formats. Packed, integer,
// typeless and UNORM vectors require another decoding path and are rejected.
unsigned int format_bytes(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16_FLOAT: return 4;
    case DXGI_FORMAT_R32G32_FLOAT: return 8;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R16G16_SNORM: return 4;
    default: return 0;
    }
}

void release_slot(DeviceState &state, Slot &slot)
{
    if (slot.view.handle != 0)
        state.device->destroy_resource_view(slot.view);
    if (slot.texture.handle != 0)
        state.device->destroy_resource(slot.texture);
    state.memory -= slot.bytes;
    slot = {};
}

bool matches_texture(const Slot &slot, DXGI_FORMAT format, std::uint32_t width, std::uint32_t height)
{
    return slot.texture.handle != 0 && slot.format == format && slot.width == width && slot.height == height;
}

bool prepare_slot(DeviceState &state, Slot &slot, const fsr_probe::Sample &sample,
                  std::uint32_t width, std::uint32_t height)
{
    if (matches_texture(slot, sample.texture.Format, width, height))
        return true;
    const std::uint64_t bytes = std::uint64_t(width) * height * format_bytes(sample.texture.Format);
    if (bytes == 0 || bytes > memory_budget)
        return false;
    // Free idle textures from the old resolution before rejecting a resize.
    for (auto &candidate : state.slots)
    {
        if (state.memory - slot.bytes <= memory_budget - bytes)
            break;
        if (&candidate != &slot && candidate.phase == Phase::free && candidate.bytes != 0 &&
            !matches_texture(candidate, sample.texture.Format, width, height))
            release_slot(state, candidate);
    }
    if (state.memory - slot.bytes > memory_budget - bytes)
        return false;
    release_slot(state, slot); // Only free/completed slots reach this function.
    const auto format = static_cast<api::format>(sample.texture.Format);
    const api::resource_desc desc(width, height, 1, 1, format, 1, api::memory_heap::default_,
                                  api::resource_usage::shader_resource | api::resource_usage::copy_dest | api::resource_usage::copy_source);
    if (!state.device->create_resource(desc, nullptr, api::resource_usage::shader_resource, &slot.texture))
        return false;
    const api::resource_view_desc view_desc(format);
    if (!state.device->create_resource_view(slot.texture, api::resource_usage::shader_resource, view_desc, &slot.view))
    {
        state.device->destroy_resource(slot.texture);
        slot.texture = {};
        return false;
    }
    slot.width = width;
    slot.height = height;
    slot.format = sample.texture.Format;
    slot.bytes = bytes;
    state.memory += bytes;
    return true;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

void capture_sample(const fsr_probe::Sample &sample) noexcept
{
    if (stopping.load(std::memory_order_relaxed))
        return;
    try
    {
        const auto command = command_id(sample.command_list);
        if (command == 0)
            return;
        const std::unique_lock lock(state_mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        const auto found = commands.find(command);
        if (found == commands.end())
            return; // Never guess the identity/queue of an unobserved list.
        const auto device_it = devices.find(found->second.device);
        if (device_it == devices.end())
            return;
        auto &state = *device_it->second;
        if (state.runtime == nullptr || state.queue == nullptr || state.queue_mismatch || !state.fence || state.present_thread == 0)
            return;
        if (sample.command_list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            ++state.rejected;
            return; // v1 deliberately does not support async-compute capture.
        }
        if (installed_reset_target.load(std::memory_order_relaxed) == nullptr)
            install_native_reset_hook(found->second.list);
        // Fallback for command-list implementations without the native hook.
        // ReShade's reset notification is emitted before native Reset returns.
        if (found->second.reset_requested)
        {
            replace_recording(state, command);
            found->second.reset_requested = false;
        }
        if (format_bytes(sample.texture.Format) == 0)
        {
            ++state.rejected;
            if (!state.warned_format)
            {
                char message[192];
                std::snprintf(message, sizeof(message), "[NativeMotion] Unsupported native MV DXGI_FORMAT=%u. No texture is interpreted; save this log for adding its decoder.", unsigned(sample.texture.Format));
                warn(message);
                state.warned_format = true;
            }
            return;
        }
        const math::extent2d render{sample.render_width, sample.render_height};
        const math::extent2d active = sample.display_resolution_motion_vectors
            ? math::extent2d{sample.upscale_width, sample.upscale_height} : render;
        const auto scale = math::fsr_render_pixel_scale({sample.scale_x, sample.scale_y}, render, active);
        if (!scale || active.width > 8192 || active.height > 8192)
        {
            ++state.rejected;
            return;
        }
        const auto completed = state.fence->GetCompletedValue();
        if (completed == UINT64_MAX) // Device removal is not a completed frame.
            return;
        Slot *slot = nullptr;
        Slot *free_slot = nullptr;
        for (auto &candidate : state.slots)
        {
            if (candidate.phase == Phase::retiring && candidate.recording_replaced && candidate.retirement <= completed)
                candidate.phase = Phase::free;
            if (candidate.phase != Phase::free)
                continue;
            if (free_slot == nullptr)
                free_slot = &candidate;
            if (slot == nullptr && matches_texture(candidate, sample.texture.Format, active.width, active.height))
                slot = &candidate;
        }
        if (slot == nullptr)
            slot = free_slot;
        if (slot == nullptr || !prepare_slot(state, *slot, sample, active.width, active.height))
        {
            ++state.dropped;
            if (slot != nullptr)
                ++state.allocation_failures;
            return; // Never wait for the GPU inside FSR dispatch.
        }
        auto *destination = reinterpret_cast<ID3D12Resource *>(slot->texture.handle);
        constexpr auto read_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        std::array barriers{
            transition(sample.motion_vectors, sample.source_state, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(destination, read_state, D3D12_RESOURCE_STATE_COPY_DEST)};
        sample.command_list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = sample.motion_vectors;
        dst.pResource = destination;
        src.Type = dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{0, 0, 0, active.width, active.height, 1};
        sample.command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
        std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
        sample.command_list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

        slot->metadata = sample;
        slot->metadata.motion_vectors = nullptr; // Borrowed pointers never escape the callback.
        slot->metadata.command_list = nullptr;
        slot->pixel_scale = *scale;
        slot->command = command;
        slot->sequence = ++state.sequence;
        slot->phase = Phase::recorded;
        slot->recording_replaced = false;
        if (state.sequence == 1 || sample.reset || (state.sequence % 600) == 0)
        {
            char message[640];
            std::snprintf(message, sizeof(message),
                "[NativeMotion] sample=%llu context=0x%llx format=%u native=%llux%u active=%ux%u render=%ux%u output=%ux%u scale=(%.6g,%.6g) pixel_scale=(%.6g,%.6g) jitter=(%.6g,%.6g) jittered=%d reset=%d frame_ms=%.6g; GPU copy recorded, awaiting submission",
                static_cast<unsigned long long>(slot->sequence), static_cast<unsigned long long>(sample.context_id),
                unsigned(sample.texture.Format), static_cast<unsigned long long>(sample.texture.Width), sample.texture.Height,
                active.width, active.height, render.width, render.height, sample.upscale_width, sample.upscale_height,
                double(sample.scale_x), double(sample.scale_y), double(scale->x), double(scale->y),
                double(sample.jitter_x), double(sample.jitter_y), int(sample.jitter_cancellation), int(sample.reset), double(sample.frame_time_ms));
            log(message);
        }
    }
    catch (...) { /* A diagnostic must never propagate an exception into FSR. */ }
}

void blur_after_upscale(const fsr_probe::Sample &sample) noexcept
{
    if (stopping.load(std::memory_order_relaxed))
        return;
    try
    {
        const std::unique_lock lock(state_mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        const auto command = command_id(sample.command_list);
        const auto tracked = commands.find(command);
        if (tracked == commands.end())
            return;
        const auto found = devices.find(tracked->second.device);
        if (found == devices.end())
            return;
        auto &state = *found->second;
        // Shared scratch is only valid on our observed, serial graphics queue.
        // New lists remain diagnostic-only until their first observed submit.
        if (!state.blur_enabled || !state.prehud || !state.runtime || state.queue_mismatch ||
            tracked->second.observed_queue != state.queue || tracked->second.mixed_queues ||
            sample.command_list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            !math::blur_sample_valid(sample.frame_time_ms, sample.reset, sample.jitter_cancellation))
            return;
        Slot *captured = nullptr;
        for (auto &slot : state.slots)
            if (slot.command == command && slot.phase == Phase::recorded && !slot.recording_replaced &&
                slot.metadata.context_id == sample.context_id &&
                slot.metadata.dispatch_id == sample.dispatch_id &&
                (captured == nullptr || slot.sequence > captured->sequence))
                captured = &slot;
        if (captured == nullptr)
            return;
        unsigned capture_index = 0;
        constexpr std::array<std::uint64_t, 4> milestones{600, 3000, 7000, 13000};
        if (blur_verification_requested())
            for (unsigned i = 0; i < milestones.size(); ++i)
                if (state.scene_captures[i].sequence == 0 && state.blur_frames >= milestones[i])
                {
                    capture_index = i + 1;
                    break;
                }
        // Use the original command list with native descriptor heaps. Passing
        // these heaps through ReShade's proxy would invoke its proxy unwrap ABI.
        auto *native_command = reinterpret_cast<ID3D12GraphicsCommandList *>(tracked->second.list->get_native());
        if (!state.prehud->record(native_command, sample,
                reinterpret_cast<ID3D12Resource *>(captured->texture.handle),
                static_cast<unsigned>(captured - state.slots.data()), state.blur_controls, capture_index))
            return;
        ++state.blur_frames;
        if (capture_index != 0)
        {
            auto &capture = state.scene_captures[capture_index - 1];
            capture.sequence = captured->sequence;
            if (!state.prehud->capture_recorded(capture_index))
            {
                capture.collected = capture.final_saved = true;
                warn("[NativeBlurPreHUD] Optional scene capture allocation failed; no paired capture claimed.");
                return;
            }
            char message[160];
            std::snprintf(message, sizeof(message), "[NativeBlurPreHUD] capture=%u sequence=%llu recorded before/after FSR output.",
                capture_index, static_cast<unsigned long long>(captured->sequence));
            log(message);
        }
    }
    catch (...) { /* Preserve FidelityFX's successful return on diagnostic failure. */ }
}

void poll_probe(bool preload_game_provider = false)
{
    if (stopping.load())
        return;
    const auto result = fsr_probe::try_install(capture_sample, preload_game_provider, blur_after_upscale);
    if (last_install.exchange(result) == result)
        return;
    switch (result)
    {
    case fsr_probe::InstallResult::early_load_failed:
        warn("[NativeMotion] Could not load the game-local FidelityFX provider during device initialization. Waiting for the game to load it."); break;
    case fsr_probe::InstallResult::installed:
        log("[NativeMotion] FSR API hooks installed. Select FSR 3 upscaling with Frame Generation off. If context was created before installation, change AA away from FSR and back."); break;
    case fsr_probe::InstallResult::unsupported:
        warn("[NativeMotion] FidelityFX module lacks the supported FSR 3.1 API. Capture disabled."); break;
    case fsr_probe::InstallResult::hook_error:
        warn("[NativeMotion] FSR hook installation failed. Capture disabled; check for competing upscaler hooks."); break;
    default: break;
    }
}

void on_init_device(api::device *device)
{
    if (device->get_api() != api::device_api::d3d12)
        return;
    {
        const std::lock_guard lock(state_mutex);
        auto state = std::make_unique<DeviceState>();
        state->device = device;
        auto *native = reinterpret_cast<ID3D12Device *>(device->get_native());
        state->prehud = std::make_unique<native_blur::Engine>(native);
        if (FAILED(native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state->fence))))
            warn("[NativeMotion] Could not create GPU retirement fence. Capture disabled.");
        devices.emplace(device, std::move(state));
    }
    log("[NativeMotion] D3D12 device detected. Waiting for amd_fidelityfx_dx12.dll and a tracked FSR context.");
    poll_probe(true);
}

void on_init_command_list(api::command_list *command)
{
    if (command->get_device()->get_api() != api::device_api::d3d12)
        return;
    const auto id = next_command.fetch_add(1);
    auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(command->get_native());
    if (SUCCEEDED(native->SetPrivateData(command_tag, sizeof(id), &id)))
    {
        const std::lock_guard lock(state_mutex);
        commands[id] = {command->get_device(), command};
    }
}

void discard_recording(api::command_list *command, bool destroy)
{
    if (command->get_device()->get_api() != api::device_api::d3d12)
        return;
    auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(command->get_native());
    const auto id = command_id(native);
    const std::lock_guard lock(state_mutex);
    if (destroy)
    {
        if (const auto found = devices.find(command->get_device()); found != devices.end())
            for (auto &slot : found->second->slots)
                if (slot.command == id)
                {
                    slot.recording_replaced = true;
                    for (auto &capture : found->second->scene_captures)
                        if (capture.sequence == slot.sequence)
                            capture.recording_replaced = true;
                    if (slot.phase == Phase::recorded)
                        slot.phase = Phase::quarantined;
                }
    }
    else if (native_reset_target(native) != installed_reset_target.load(std::memory_order_acquire))
    {
        if (const auto found = commands.find(id); found != commands.end())
            found->second.reset_requested = true;
    }
    if (destroy)
        commands.erase(id);
}
void on_reset_command_list(api::command_list *command) { discard_recording(command, false); }
void on_destroy_command_list(api::command_list *command) { discard_recording(command, true); }

void on_execute(api::command_queue *queue, api::command_list *command)
{
    if (queue->get_device()->get_api() != api::device_api::d3d12)
        return;
    const auto id = command_id(reinterpret_cast<ID3D12GraphicsCommandList *>(command->get_native()));
    const std::lock_guard lock(state_mutex);
    if (const auto tracked = commands.find(id); tracked != commands.end())
    {
        if (tracked->second.observed_queue != nullptr && tracked->second.observed_queue != queue)
            tracked->second.mixed_queues = true;
        tracked->second.observed_queue = queue;
    }
    const auto found = devices.find(queue->get_device());
    if (found == devices.end())
        return;
    auto &state = *found->second;
    for (auto &slot : state.slots)
    {
        if (slot.command != id || slot.recording_replaced || slot.phase == Phase::free || slot.phase == Phase::quarantined)
            continue;
        if (queue != state.queue || GetCurrentThreadId() != state.present_thread)
        {
            slot.phase = Phase::quarantined;
            if (!state.queue_mismatch)
                warn("[NativeMotion] FSR copy submitted on a different queue or presentation thread. Capture disabled for this device; async/multithread submissions are not supported by v1.");
            state.queue_mismatch = true;
        }
        else
        {
            slot.phase = Phase::submitted;
            for (auto &capture : state.scene_captures)
                if (capture.sequence == slot.sequence && ++capture.submissions == 2)
                    warn("[NativeBlurPreHUD] Captured recording replayed; composed screenshot is not a guaranteed same-frame match.");
        }
    }
}

void on_init_runtime(api::effect_runtime *runtime)
{
    if (runtime->get_device()->get_api() != api::device_api::d3d12)
        return;
    const std::lock_guard lock(state_mutex);
    const auto found = devices.find(runtime->get_device());
    if (found == devices.end())
        return;
    auto &state = *found->second;
    if (state.runtime != nullptr && state.runtime != runtime)
        return; // Never mix two swap chains' scene data.
    if (state.queue != nullptr && state.queue != runtime->get_command_queue())
    {
        state.queue_mismatch = true;
        warn("[NativeMotion] Graphics queue changed during runtime recreation. Restart required; the previous queue's fence is not transferable.");
    }
    state.runtime = runtime;
    state.queue = runtime->get_command_queue();
    state.binding_dirty = true;
    log("[NativeMotion] Runtime attached. Enable NativeMotionBlur.fx for blur or NativeMotionVectors.fx for diagnostics in ReShade.");
}

api::effect_uniform_variable find_uniform(api::effect_runtime *runtime, const char *name, const char *file = effect_file)
{
    // Avoid virtual struct-by-value returns: LLVM-MinGW and MSVC disagree on
    // that ABI even for ReShade's eight-byte handles. Callback values are safe.
    api::effect_uniform_variable result{};
    runtime->enumerate_uniform_variables(file, [&](api::effect_runtime *owner, api::effect_uniform_variable variable) {
        char candidate[128]{};
        owner->get_uniform_variable_name(variable, candidate);
        if (std::strcmp(candidate, name) == 0)
            result = variable;
    });
    return result;
}
void set_valid(api::effect_runtime *runtime, bool valid)
{
    for (const auto file : effect_files)
    {
        const auto uniform = find_uniform(runtime, "NativeMotionValid", file);
        if (uniform.handle != 0)
            runtime->set_uniform_value_bool(uniform, &valid, 1);
    }
}
void set_float2(api::effect_runtime *runtime, const char *name, float x, float y)
{
    const float values[]{x, y};
    for (const auto file : effect_files)
    {
        const auto uniform = find_uniform(runtime, name, file);
        if (uniform.handle != 0)
            runtime->set_uniform_value_float(uniform, values, 2);
    }
}

void on_begin_effects(api::effect_runtime *runtime, api::command_list *command, api::resource_view, api::resource_view)
{
    if (runtime->get_device()->get_api() != api::device_api::d3d12)
        return;
    const std::lock_guard lock(state_mutex);
    const auto found = devices.find(runtime->get_device());
    if (found == devices.end() || found->second->runtime != runtime)
        return;
    auto &state = *found->second;
    state.blur_frame_rendered = false;
    Slot *latest = nullptr;
    if (!state.queue_mismatch && GetCurrentThreadId() == state.present_thread &&
        command == state.queue->get_immediate_command_list())
        for (auto &slot : state.slots)
            if (slot.phase == Phase::submitted && slot.sequence > state.displayed &&
                (latest == nullptr || slot.sequence > latest->sequence))
                latest = &slot;
    bool valid = latest != nullptr;
    if (valid)
    {
        const bool resize = state.display.texture.handle == 0 || state.display.width != latest->width ||
            state.display.height != latest->height || state.display.format != latest->format;
        if (resize)
        {
            // Binding updates wait for the graphics queue in ReShade. Use
            // them only for allocation/reload, never once per rendered frame.
            if (state.display.texture.handle != 0)
            {
                runtime->update_texture_bindings(semantic, {}, {});
                state.queue->wait_idle();
            }
            valid = prepare_slot(state, state.display, latest->metadata, latest->width, latest->height);
            state.binding_dirty = true;
        }
        if (valid && state.binding_dirty)
        {
            runtime->update_texture_bindings(semantic, state.display.view, state.display.view);
            state.binding_dirty = false;
        }
        if (valid)
        {
            // Use ReShade's API on its immediate list so it records that this
            // list contains work even when no effect technique is enabled.
            command->barrier(latest->texture, api::resource_usage::shader_resource, api::resource_usage::copy_source);
            command->barrier(state.display.texture, api::resource_usage::shader_resource, api::resource_usage::copy_dest);
            command->copy_resource(latest->texture, state.display.texture);
            command->barrier(latest->texture, api::resource_usage::copy_source, api::resource_usage::shader_resource);
            command->barrier(state.display.texture, api::resource_usage::copy_dest, api::resource_usage::shader_resource);
        }
    }
    set_valid(runtime, valid);
    // Never run the old post-HUD gather, including when native insertion skips.
    const bool blur_valid = false;
    const auto blur_valid_uniform = find_uniform(runtime, "NativeMotionValid", blur_effect_file);
    if (blur_valid_uniform.handle != 0)
        runtime->set_uniform_value_bool(blur_valid_uniform, &blur_valid, 1);
    state.last_blur_valid = blur_valid;
    if (valid)
    {
        set_float2(runtime, "NativeMotionScale", latest->pixel_scale.x, latest->pixel_scale.y);
        set_float2(runtime, "NativeMotionRenderSize", float(latest->metadata.render_width), float(latest->metadata.render_height));
        set_float2(runtime, "NativeMotionTextureSize", float(latest->width), float(latest->height));
        set_float2(runtime, "NativeMotionUVScale", 1.f, 1.f);
        const auto timing = find_uniform(runtime, "NativeMotionFrameTimeMs", blur_effect_file);
        if (timing.handle != 0)
            runtime->set_uniform_value_float(timing, &latest->metadata.frame_time_ms, 1);
        state.displayed = latest->sequence;
    }
    if (valid != state.last_valid)
    {
        log(valid ? "[NativeMotion] LIVE: fresh FSR motion vectors bound to the visualization."
                  : "[NativeMotion] NO FRESH SAMPLE: visualization disabled; previous vectors will not be reused.");
        state.last_valid = valid;
    }
}

bool verification_requested()
{
    static const bool enabled = [] {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"GTA_NATIVE_MOTION_CAPTURE", value, 2) == 1 && value[0] == L'1';
    }();
    return enabled;
}

bool blur_verification_requested()
{
    static const bool enabled = [] {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"GTA_NATIVE_BLUR_CAPTURE", value, 2) == 1 && value[0] == L'1';
    }();
    return enabled;
}

bool direct_screenshot_target(api::effect_runtime *runtime)
{
    // ReShade's screenshot API assumes PRESENT for this direct SDR target.
    // Resolved/MSAA/X8 targets use different states and are excluded.
    ComPtr<IDXGISwapChain> swapchain;
    DXGI_SWAP_CHAIN_DESC desc{};
    auto *native = reinterpret_cast<IUnknown *>(runtime->get_native());
    return SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&swapchain))) && SUCCEEDED(swapchain->GetDesc(&desc)) &&
        desc.SampleDesc.Count == 1 &&
        (desc.BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.BufferDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
}

void refresh_blur_controls(DeviceState &state)
{
    auto *runtime = state.runtime;
    bool technique_enabled = false;
    runtime->enumerate_techniques(blur_effect_file, [&](api::effect_runtime *owner, api::effect_technique technique) {
        char name[64]{};
        owner->get_technique_name(technique, name);
        if (std::strcmp(name, blur_technique_name) == 0)
            technique_enabled = owner->get_technique_state(technique);
    });
    bool complete = true;
    auto read_int = [&](const char *name, int &value) {
        const auto uniform = find_uniform(runtime, name, blur_effect_file);
        if (uniform.handle == 0) complete = false;
        else runtime->get_uniform_value_int(uniform, &value, 1);
    };
    auto read_float = [&](const char *name, float &value) {
        const auto uniform = find_uniform(runtime, name, blur_effect_file);
        if (uniform.handle == 0) complete = false;
        else runtime->get_uniform_value_float(uniform, &value, 1);
    };
    auto &controls = state.blur_controls;
    read_int("NativeMotionBlurExposureMode", controls.exposure_mode);
    read_int("NativeMotionBlurSamples", controls.samples);
    read_int("NativeMotionBlurDisplay", controls.display);
    read_float("NativeMotionBlurShutterAngle", controls.shutter_angle);
    read_float("NativeMotionBlurShutterMs", controls.shutter_ms);
    read_float("NativeMotionBlurMaxRadiusPx", controls.max_radius);
    const bool enabled = complete && technique_enabled && runtime->get_effects_state();
    if (enabled != state.blur_enabled)
        log(enabled ? "[NativeBlurPreHUD] Enabled: scene blur after FSR, before HUD."
                    : "[NativeBlurPreHUD] Disabled or controls unavailable; no post-HUD fallback.");
    state.blur_enabled = enabled;
}

void on_present(api::command_queue *queue, api::swapchain *swapchain,
                const api::rect *, const api::rect *, uint32_t, const api::rect *)
{
    if (queue->get_device()->get_api() != api::device_api::d3d12)
        return;
    api::effect_runtime *runtime = nullptr;
    unsigned shot = 0;
    std::uint64_t sequence = 0;
    {
        const std::lock_guard lock(state_mutex);
        const auto found = devices.find(queue->get_device());
        if (found == devices.end())
            return;
        auto &state = *found->second;
        if (state.runtime == nullptr || state.runtime->get_native() != swapchain->get_native() ||
            state.queue != queue || state.queue_mismatch)
            return;
        // This callback runs even while F7 globally disables ReShade effects.
        refresh_blur_controls(state);
        if (!blur_verification_requested())
            return;
        for (unsigned i = 0; i < state.scene_captures.size(); ++i)
        {
            auto &capture = state.scene_captures[i];
            if (capture.sequence == 0 || capture.final_saved)
                continue;
            for (const auto &slot : state.slots)
                if (slot.sequence == capture.sequence && slot.phase == Phase::submitted)
                {
                    capture.final_saved = true;
                    runtime = state.runtime;
                    sequence = capture.sequence;
                    shot = i + 1;
                    break;
                }
            if (shot != 0) break;
        }
    }
    if (shot != 0 && direct_screenshot_target(runtime))
    {
        char postfix[64], message[160];
        std::snprintf(postfix, sizeof(postfix), "NativePreHUD-%02u-Final", shot);
        std::snprintf(message, sizeof(message), "[NativeBlurPreHUD] capture=%u sequence=%llu saving composed final image.",
            shot, static_cast<unsigned long long>(sequence));
        log(message);
        runtime->save_screenshot(postfix);
    }
}

void on_render_technique(api::effect_runtime *runtime, api::effect_technique technique,
                         api::command_list *command, api::resource_view, api::resource_view)
{
    const std::lock_guard lock(state_mutex);
    const auto found = devices.find(runtime->get_device());
    if (found == devices.end() || found->second->runtime != runtime)
        return;
    auto &state = *found->second;
    if (command != state.queue->get_immediate_command_list())
        return;
    char name[64]{}, file[64]{};
    runtime->get_technique_name(technique, name);
    runtime->get_technique_effect_name(technique, file);
    if (std::strcmp(name, blur_technique_name) == 0 && std::strcmp(file, blur_effect_file) == 0)
    {
        if (!state.blur_technique_rendered)
        {
            log("[NativeMotion] NativeMotionBlur controls are active; presentation pass is a passthrough.");
            state.blur_technique_rendered = true;
        }
        return;
    }
    if (std::strcmp(name, technique_name) != 0 || std::strcmp(file, effect_file) != 0)
        return;
    if (!state.technique_rendered)
    {
        log("[NativeMotion] NativeMotionVisualization technique is active and rendering.");
        state.technique_rendered = true;
    }
    if (verification_requested() && state.verification_frames < verification_frame_count &&
        state.last_valid && GetCurrentThreadId() == state.present_thread)
        state.verification_frame_rendered = true;
}

void on_reshade_present(api::effect_runtime *runtime)
{
    bool diagnostic_shot = false;
    {
        const std::lock_guard lock(state_mutex);
        const auto found = devices.find(runtime->get_device());
        if (found == devices.end() || found->second->runtime != runtime)
            return;
        auto &state = *found->second;
        if (verification_requested() && std::exchange(state.verification_frame_rendered, false))
            diagnostic_shot = ++state.verification_frames == verification_frame_count;
    }
    if (diagnostic_shot && direct_screenshot_target(runtime))
        runtime->save_screenshot("NativeMotion-live");
}

void collect_scene_capture(DeviceState &state, unsigned index)
{
    auto &pending = state.scene_captures[index - 1];
    native_blur::Capture capture;
    pending.collected = true;
    if (!state.prehud->collect(index, capture))
    {
        warn("[NativeBlurPreHUD] Scene readback unavailable; do not treat it as a paired capture.");
        return;
    }
    try
    {
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        const auto directory = std::filesystem::path(executable).parent_path() / L"NativeMotionCaptures";
        std::filesystem::create_directories(directory);
        char stem[64];
        std::snprintf(stem, sizeof(stem), "NativePreHUD-%02u", index);
        for (const bool after : {false, true})
        {
            const auto &pixels = after ? capture.after : capture.before;
            std::ofstream out(directory / (std::string(stem) + (after ? "-SceneAfter.bin" : "-SceneBefore.bin")), std::ios::binary);
            out.write(reinterpret_cast<const char *>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
            if (!out) throw std::runtime_error("Scene pixel file write failed");
        }
        std::ofstream metadata(directory / (std::string(stem) + ".json"));
        metadata << "{\n  \"width\": " << capture.width << ",\n  \"height\": " << capture.height
                 << ",\n  \"dxgi_format\": " << unsigned(capture.format)
                 << ",\n  \"sequence\": " << pending.sequence
                 << ",\n  \"submissions\": " << pending.submissions
                 << ",\n  \"final_same_frame\": " << (pending.final_saved && pending.submissions == 1 ? "true" : "false")
                 << "\n}\n";
        if (!metadata) throw std::runtime_error("Scene metadata file write failed");
        char message[160];
        std::snprintf(message, sizeof(message), "[NativeBlurPreHUD] capture=%u scene readbacks saved (%ux%u format=%u).",
            index, capture.width, capture.height, unsigned(capture.format));
        log(message);
    }
    catch (...) { warn("[NativeBlurPreHUD] Failed to write scene capture files."); }
}

// ReShade 6.8's finish_present runs after the actual DXGI Present. Its effect
// commands have been submitted by this point. Retire copies only on that same
// queue; no guessed time-based lifetime, CPU readback or per-frame GPU stall.
void on_finish_present(api::command_queue *queue, api::swapchain *swapchain)
{
    if (queue->get_device()->get_api() != api::device_api::d3d12)
        return;
    {
        const std::lock_guard lock(state_mutex);
        const auto found = devices.find(queue->get_device());
        if (found == devices.end())
            return;
        auto &state = *found->second;
        if (state.runtime == nullptr || state.runtime->get_native() != swapchain->get_native() ||
            state.queue != queue || !state.fence)
            return;
        if (state.present_thread == 0)
            state.present_thread = GetCurrentThreadId();
        else if (state.present_thread != GetCurrentThreadId())
        {
            state.queue_mismatch = true;
            warn("[NativeMotion] Presentation moved to another thread. Capture stopped to preserve submission ordering.");
        }
        if (state.queue_mismatch)
            return;
        const auto value = ++state.fence_value;
        auto *native = reinterpret_cast<ID3D12CommandQueue *>(queue->get_native());
        if (FAILED(native->Signal(state.fence.Get(), value)))
        {
            state.queue_mismatch = true;
            warn("[NativeMotion] GPU fence signal failed. Capture stopped.");
            return;
        }
        for (auto &slot : state.slots)
            if (slot.phase == Phase::submitted)
            {
                for (auto &capture : state.scene_captures)
                    if (capture.sequence == slot.sequence)
                        capture.retirement = value;
                slot.phase = Phase::retiring;
                slot.retirement = value;
            }
        const auto completed_capture = state.fence->GetCompletedValue();
        if (completed_capture != UINT64_MAX)
            for (unsigned i = 0; i < state.scene_captures.size(); ++i)
            {
                const auto &capture = state.scene_captures[i];
                if (!capture.collected && capture.recording_replaced &&
                    capture.retirement != 0 && capture.retirement <= completed_capture)
                    collect_scene_capture(state, i + 1);
            }
        if ((++state.frames % 600) == 0)
        {
            const auto stats = fsr_probe::get_stats();
            const auto completed = state.fence->GetCompletedValue();
            unsigned held_reset = 0, in_flight = 0, recorded = 0, quarantined = 0;
            for (const auto &slot : state.slots)
            {
                if (slot.phase == Phase::retiring)
                {
                    if (completed == UINT64_MAX || slot.retirement > completed)
                        ++in_flight;
                    else if (!slot.recording_replaced)
                        ++held_reset;
                }
                else if (slot.phase == Phase::recorded)
                    ++recorded;
                else if (slot.phase == Phase::quarantined)
                    ++quarantined;
            }
            char message[512];
            std::snprintf(message, sizeof(message),
                "[NativeMotion] status: dispatch=%llu accepted=%llu unknown_context=%llu unsupported=%llu copies=%llu shown=%llu dropped=%llu rejected=%llu bytes=%llu blur_frames=%llu allocation_failures=%llu held_reset=%u in_flight=%u recorded=%u quarantined=%u",
                static_cast<unsigned long long>(stats.upscale_dispatches), static_cast<unsigned long long>(stats.samples),
                static_cast<unsigned long long>(stats.untracked_contexts), static_cast<unsigned long long>(stats.unsupported_samples),
                static_cast<unsigned long long>(state.sequence), static_cast<unsigned long long>(state.displayed),
                static_cast<unsigned long long>(state.dropped), static_cast<unsigned long long>(state.rejected),
                static_cast<unsigned long long>(state.memory), static_cast<unsigned long long>(state.blur_frames),
                static_cast<unsigned long long>(state.allocation_failures),
                held_reset, in_flight, recorded, quarantined);
            log(message);
            if (state.prehud)
            {
                const auto native = state.prehud->stats();
                std::snprintf(message, sizeof(message),
                    "[NativeBlurPreHUD] status: recorded=%llu skipped=%llu allocation_failures=%llu enabled=%d display=%d shutter_angle=%.3f samples=%d radius=%.3f",
                    static_cast<unsigned long long>(native.drawn), static_cast<unsigned long long>(native.skipped),
                    static_cast<unsigned long long>(native.allocation_failures), int(state.blur_enabled),
                    state.blur_controls.display, double(state.blur_controls.shutter_angle), state.blur_controls.samples,
                    double(state.blur_controls.max_radius));
                log(message);
            }
        }
    }
    poll_probe();
}

void on_destroy_runtime(api::effect_runtime *runtime)
{
    const std::lock_guard lock(state_mutex);
    const auto found = devices.find(runtime->get_device());
    if (found != devices.end() && found->second->runtime == runtime)
    {
        set_valid(runtime, false);
        runtime->update_texture_bindings(semantic, {}, {});
        // A runtime reset is infrequent. Drain its effects before the display
        // allocation is detached; captures remain protected by their fences.
        found->second->queue->wait_idle();
        found->second->runtime = nullptr;
        found->second->blur_enabled = false;
        found->second->binding_dirty = true;
        found->second->technique_rendered = false;
        found->second->verification_frame_rendered = false;
        found->second->last_blur_valid = false;
        found->second->blur_frame_rendered = false;
        found->second->capture_pair_pending = false;
        // Keep owned copies alive across swapchain recreation. They are only
        // reused after fences complete, or destroyed with their device.
    }
}

void on_destroy_device(api::device *device)
{
    const std::lock_guard lock(state_mutex);
    const auto found = devices.find(device);
    if (found == devices.end())
        return;
    auto &state = *found->second;
    const auto completed = state.fence ? state.fence->GetCompletedValue() : 0;
    bool retained = false;
    for (auto &slot : state.slots)
    {
        if (slot.phase == Phase::free ||
            (slot.phase == Phase::retiring && completed != UINT64_MAX && slot.retirement <= completed))
            release_slot(state, slot);
        else if (slot.texture.handle != 0)
            retained = true; // No proven completion: keep the bounded allocation until process exit.
    }
    if (state.display.texture.handle != 0)
    {
        if (state.runtime == nullptr && !state.queue_mismatch)
            release_slot(state, state.display); // destroy_runtime drained the graphics queue.
        else
            retained = true;
    }
    if (retained)
        warn("[NativeMotion] Retained diagnostic allocations without proven completion until process exit. Restart the game before another capture after an unsupported queue/device transition.");
    if (retained || state.runtime != nullptr || state.queue_mismatch)
        (void)state.prehud.release(); // Do not release native allocations with unproven GPU completion.
    devices.erase(found);
    for (auto it = commands.begin(); it != commands.end();)
        it = it->second.device == device ? commands.erase(it) : std::next(it);
}

void on_reloaded_effects(api::effect_runtime *runtime)
{
    const std::lock_guard lock(state_mutex);
    const auto found = devices.find(runtime->get_device());
    if (found != devices.end() && found->second->runtime == runtime)
        found->second->binding_dirty = true;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        if (!reshade::register_addon(module))
            return FALSE;
        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
        reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
        reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
        reshade::register_event<reshade::addon_event::execute_command_list>(on_execute);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_runtime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_runtime);
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
        reshade::register_event<reshade::addon_event::reshade_render_technique>(on_render_technique);
        reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
        reshade::register_event<reshade::addon_event::present>(on_present);
        reshade::register_event<reshade::addon_event::finish_present>(on_finish_present);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        stopping.store(true);
        fsr_probe::stop_capture();
        reshade::unregister_addon(module);
    }
    return TRUE;
}
