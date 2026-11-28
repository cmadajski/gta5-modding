#include "wheel_runtime.hpp"

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <numbers>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wheel_arc_math.hpp"
#include "wheel_descriptors.hpp"
#include "wheel_mask.hpp"
#include "wheel_motion_history.hpp"
#include "wheel_pipeline_hooks.hpp"

using Microsoft::WRL::ComPtr;

namespace wheel_runtime
{
namespace
{
namespace math = gtav::wheel_motion;
namespace arc = gtav::wheel_arc;
constexpr std::uint32_t slice_size = sizeof(arc::WheelGpuConstants);
constexpr std::uint32_t pool_size = 32 * 1024 * 1024;
constexpr std::uint32_t slice_count = pool_size / slice_size;
static_assert(slice_size == 512 && slice_size % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0);
static_assert(slice_count == wheel_mask::id_scale);
constexpr std::uint32_t no_slice = slice_count;
constexpr unsigned max_recordings = 512, max_draws = 256, max_commands = 4096, max_uploads = 4096;
constexpr unsigned max_history = 2048;
constexpr unsigned coverage_history = 256;
constexpr DWORD replay_wait_ms = 500;
constexpr auto gpu_upload_heap = static_cast<D3D12_HEAP_TYPE>(5);
constexpr math::FitLimits fit_limits{
    .max_origin_delta_pixels = 2.0,
    .max_roll_radians = std::numbers::pi_v<double>,
};

struct Constants
{
    math::Matrix world, current, previous;
};
static_assert(sizeof(Constants) == 192);

struct Mesh
{
    ID3D12Resource *index_buffer;
    std::uint64_t offset;
    UINT count, first;
    INT base;
    UINT instance;
    bool operator==(const Mesh &) const = default;
};

struct Target
{
    ID3D12Resource *motion_rt;
    float x, y;
    std::uint32_t width, height;
    bool operator==(const Target &) const = default;
};

struct Upload
{
    D3D12_GPU_VIRTUAL_ADDRESS address;
    std::uint64_t size;
};

struct Source
{
    ComPtr<ID3D12Resource> resource;
    const std::byte *bytes = nullptr;

    Source() = default;
    Source(Source &&other) noexcept : resource(std::move(other.resource)), bytes(std::exchange(other.bytes, nullptr)) {}
    ~Source()
    {
        constexpr D3D12_RANGE no_writes{};
        if (bytes != nullptr)
            resource->Unmap(0, &no_writes);
    }

    bool map(ID3D12Resource *buffer, SIZE_T offset)
    {
        resource = buffer;
        const D3D12_RANGE reads{offset, offset + sizeof(Constants)};
        void *data = nullptr;
        if (FAILED(buffer->Map(0, &reads, &data)))
            return false;
        if (data == nullptr)
        {
            constexpr D3D12_RANGE no_writes{};
            buffer->Unmap(0, &no_writes);
            return false;
        }
        bytes = static_cast<const std::byte *>(data) + offset;
        return true;
    }
};

enum class RecordDecision
{
    no_history,
    unsafe_seed,
    pool_full,
    override_failed,
    substituted,
};

struct Draw
{
    Mesh mesh{};
    Target target{};
    D3D12_CPU_DESCRIPTOR_HANDLE cbv{};
    wheel_descriptors::ConstantBufferView descriptor{};
    Source source;
    unsigned slice = no_slice;
    RecordDecision decision = RecordDecision::no_history;
    bool arc_enabled = false;
    std::shared_ptr<const wheel_pipeline_hooks::Match> pipeline;
};

struct Recording
{
    std::vector<Draw> draws;
    std::uint64_t last_fence = 0;
    unsigned submissions = 0;
    bool has_private_data = false, quarantine = false;
    std::shared_ptr<wheel_mask::Surface> arc_surface;
    bool arc_pending = false;
};

struct Observation
{
    Mesh mesh;
    Target target;
    math::Matrix current;
};

struct HistoryClaim
{
    const math::Matrix *prior;
    math::Matrix current;
};

struct ArcIdentity
{
    math::Matrix current;
    Target target;
};

using Record = std::shared_ptr<Recording>;
using ReleasedRecords = std::array<Record, max_recordings>;

struct State
{
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ComPtr<ID3D12Resource> pool;
    ComPtr<ID3D12Fence> fence;
    HANDLE replay_event = nullptr;
    std::byte *mapping = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS pool_address = 0;
    std::mutex mutex;
    std::uint64_t next_fence = 0, present_fence_cutoff = 0;
    DWORD present_thread = 0;
    bool submitting = false, interval_overflow = false, enabled = true, have_frame = false;
    Frame frame;
    Target verified_target{};
    std::uint64_t verified_target_frame = 0;
    std::atomic<std::uint64_t> attempted{0};
    Stats counters;
    Coverage interval_coverage;
    std::array<std::optional<FrameCoverage>, coverage_history> recent_coverage;
    std::vector<unsigned> free_slices;
    std::unordered_map<ID3D12Resource *, Upload> uploads;
    std::unordered_map<ID3D12CommandList *, bool> exclusive_primary;
    std::unordered_map<ID3D12CommandList *, Record> active;
    std::vector<Record> retired;
    std::vector<Observation> interval, previous;
    std::vector<HistoryClaim> interval_claims;
    std::vector<ArcIdentity> arc_identities;

    ~State()
    {
        if (mapping != nullptr)
            pool->Unmap(0, nullptr);
        if (replay_event != nullptr)
            CloseHandle(replay_event);
    }
};

using Execute = void (STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
struct Process
{
    std::mutex initialize, state_mutex;
    std::shared_ptr<State> active;
    ID3D12Device *pending_device = nullptr;
    std::unordered_map<ID3D12Resource *, Upload> pending_uploads;
    std::atomic<Execute> original{};
    void *target = nullptr;
    HMODULE module = nullptr;
};

Process &process()
{
    // Detours and quarantined allocations can outlive an observed device
    // callback. Releasing COM objects under the DLL loader lock is avoided.
    static auto *value = new Process;
    return *value;
}

std::shared_ptr<State> state_for(ID3D12Device *device)
{
    auto &p = process();
    const std::lock_guard lock(p.state_mutex);
    return p.active && p.active->device == device ? p.active : nullptr;
}

std::shared_ptr<State> current_state()
{
    auto &p = process();
    const std::lock_guard lock(p.state_mutex);
    return p.active;
}

bool finite(const math::Matrix &matrix)
{
    return std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); });
}

bool valid_frame(const Frame &frame)
{
    return frame.valid && !frame.reset && !frame.jitter_cancellation && frame.context_id != 0 && frame.dispatch_id != 0 &&
        frame.motion_rt != nullptr &&
        frame.render_width != 0 && frame.render_height != 0 && frame.upscale_width != 0 && frame.upscale_height != 0 &&
        std::isfinite(frame.jitter_x) && std::isfinite(frame.jitter_y);
}

bool matches_frame(const Target &target, const Frame &frame)
{
    return valid_frame(frame) && target.motion_rt == frame.motion_rt && target.x == 0 && target.y == 0 &&
        target.width == frame.render_width && target.height == frame.render_height;
}

bool recently_verified_target(const State &state, const Target &target)
{
    return state.have_frame && state.frame.frame_id >= state.verified_target_frame &&
        state.frame.frame_id - state.verified_target_frame <= 1 && target == state.verified_target;
}

bool close_origin(const math::Matrix &stock, const math::Matrix &candidate, const Target &target)
{
    const double sw = stock[15], cw = candidate[15];
    if (!(sw > 0 && cw > 0) || std::abs(sw - cw) > fit_limits.max_clip_w_relative_delta * std::max(sw, cw))
        return false;
    const double dx = (stock[12] / sw - candidate[12] / cw) * target.width * 0.5;
    const double dy = (stock[13] / sw - candidate[13] / cw) * target.height * 0.5;
    const double dz = stock[14] / sw - candidate[14] / cw;
    return std::hypot(dx, dy) <= fit_limits.max_origin_delta_pixels && std::abs(dz) <= fit_limits.max_ndc_depth_delta;
}

struct PriorMatch
{
    const math::Matrix *matrix = nullptr;
    bool has_mesh = false, ambiguous = false;
};

PriorMatch prior_matrix(const State &state, const Draw &draw, const math::Matrix &previous)
{
    PriorMatch match;
    for (const auto &prior : state.previous)
    {
        if (prior.mesh != draw.mesh || prior.target != draw.target)
            continue;
        match.has_mesh = true;
        if (!close_origin(previous, prior.current, draw.target))
            continue;
        if (match.matrix != nullptr && *match.matrix != prior.current)
            return {nullptr, true, true};
        match.matrix = &prior.current;
    }
    return match;
}

void remember(State &state, const Draw &draw, const math::Matrix &current)
{
    if (!finite(current) || state.interval_overflow)
        return;
    const auto duplicate = [&](const Observation &o) {
        return o.mesh == draw.mesh && o.target == draw.target && o.current == current;
    };
    if (std::any_of(state.interval.begin(), state.interval.end(), duplicate))
        return;
    if (state.interval.size() == max_history)
    {
        state.interval_overflow = true;
        return;
    }
    state.interval.push_back({draw.mesh, draw.target, current});
}

void read_constants(const Draw &draw, Constants &constants)
{
    std::memcpy(&constants, draw.source.bytes, sizeof(constants));
}

bool resolve_source(State &state, Draw &draw)
{
    const auto desc = wheel_descriptors::lookup(state.device, draw.cbv);
    if (!desc || desc->size < sizeof(Constants))
        return false;
    ID3D12Resource *source = nullptr;
    std::uint64_t offset = 0;
    for (const auto &[resource, upload] : state.uploads)
    {
        if (desc->gpu_address < upload.address)
            continue;
        const auto candidate = desc->gpu_address - upload.address;
        if (candidate > upload.size || desc->size > upload.size - candidate ||
            candidate > SIZE_MAX - sizeof(Constants))
            continue;
        if (source != nullptr)
            return false; // Placed/aliased buffers can share a virtual range.
        source = resource;
        offset = candidate;
    }
    if (source == nullptr || !draw.source.map(source, static_cast<SIZE_T>(offset)))
        return false;
    draw.descriptor = *desc;
    return true;
}

bool claim_history(State &state, const math::Matrix *prior, const math::Matrix &current)
{
    const auto existing = std::find_if(state.interval_claims.begin(), state.interval_claims.end(), [&](const HistoryClaim &claim) {
        return claim.prior == prior;
    });
    if (existing != state.interval_claims.end())
        return existing->current == current;
    state.interval_claims.push_back({prior, current});
    return true;
}

void record_coverage(State &state, std::uint64_t Coverage::*count)
{
    ++(state.interval_coverage.*count);
    ++(state.counters.coverage.*count);
}

void report_submission(State &state, RecordDecision decision)
{
    record_coverage(state, &Coverage::observed);
    switch (decision)
    {
    case RecordDecision::no_history: record_coverage(state, &Coverage::record_no_history); break;
    case RecordDecision::unsafe_seed: record_coverage(state, &Coverage::record_unsafe_seed); break;
    case RecordDecision::pool_full: record_coverage(state, &Coverage::record_pool_full); break;
    case RecordDecision::override_failed: record_coverage(state, &Coverage::record_override_failed); break;
    case RecordDecision::substituted: record_coverage(state, &Coverage::submitted); break;
    }
}

std::optional<double> correct_previous(State &state, const Draw &draw, Constants &stock)
{
    if (!matches_frame(draw.target, state.frame) || state.previous.empty())
    {
        record_coverage(state, &Coverage::missing_history);
        return std::nullopt;
    }
    const auto prior = prior_matrix(state, draw, stock.previous);
    if (prior.matrix == nullptr)
    {
        const auto reason = prior.ambiguous ? &Coverage::ambiguous_prior
            : prior.has_mesh ? &Coverage::missing_prior : &Coverage::missing_prior_mesh;
        record_coverage(state, reason);
        return std::nullopt;
    }
    const auto fit = math::fit_previous_wvp(stock.previous, *prior.matrix, draw.target.width, draw.target.height, fit_limits);
    if (!fit.corrected || !finite(stock.current))
    {
        record_coverage(state, fit.stationary && finite(stock.current) ? &Coverage::stationary : &Coverage::rejected_fit);
        return std::nullopt;
    }
    if (!claim_history(state, prior.matrix, stock.current))
    {
        record_coverage(state, &Coverage::claim_conflict);
        return std::nullopt;
    }
    stock.previous = fit.previous_wvp;
    ++state.counters.corrected;
    record_coverage(state, &Coverage::corrected);
    return fit.roll_radians;
}

std::uint32_t canonical_arc(State &state, const Target &target, const math::Matrix &current)
{
    const auto equal = [&](const ArcIdentity &entry) {
        return entry.target == target && std::memcmp(entry.current.data(), current.data(), sizeof(current)) == 0;
    };
    const auto found = std::find_if(state.arc_identities.begin(), state.arc_identities.end(), equal);
    if (found != state.arc_identities.end())
        return static_cast<std::uint32_t>(found - state.arc_identities.begin()) + 1;
    if (state.arc_identities.size() == max_history)
        return 0;
    state.arc_identities.push_back({current, target});
    return static_cast<std::uint32_t>(state.arc_identities.size());
}

void fill_before_submit(State &state, Draw &draw, bool replay)
{
    const auto desc = wheel_descriptors::lookup(state.device, draw.cbv);
    const bool same_binding = desc && desc->generation == draw.descriptor.generation &&
        desc->gpu_address == draw.descriptor.gpu_address && desc->size == draw.descriptor.size;
    Constants stock;
    // The verified b11 range is a static descriptor. Its address cannot change
    // while recording can still execute, but the upload payload may change
    // before each execution. Keep a nested Map to reread the original address.
    read_constants(draw, stock);

    if (!replay)
    {
        report_submission(state, draw.decision);
        if (draw.arc_enabled) record_coverage(state, &Coverage::arc_submitted);
        if (same_binding)
            remember(state, draw, stock.current);
    }
    if (draw.slice == no_slice)
        return;
    const auto stock_previous = stock.previous;
    std::optional<double> roll;
    if (!same_binding)
    {
        ++state.counters.source_fallbacks;
        if (!replay) record_coverage(state, &Coverage::descriptor_mismatch);
    }
    else if (!replay)
        roll = correct_previous(state, draw, stock);

    auto *slice = state.mapping + draw.slice * slice_size;
    if (!draw.arc_enabled)
    {
        std::memcpy(slice, &stock, sizeof(stock));
        return;
    }

    arc::WheelGpuConstants constants{};
    constants.world = stock.world;
    constants.current = stock.current;
    constants.corrected_previous = stock.previous;
    constants.stock_previous = stock_previous;
    constants.pool_index = draw.slice;
    constants.render_width = draw.target.width;
    constants.render_height = draw.target.height;
    if (roll && state.have_frame && state.frame.frame_id < UINT32_MAX)
    {
        constants.frame_id = static_cast<std::uint32_t>(state.frame.frame_id + 1);
        constants.canonical_id = canonical_arc(state, draw.target, stock.current);
        if (constants.canonical_id != 0 && arc::prepare_constants(constants, *roll))
            record_coverage(state, &Coverage::arc_valid);
        else
            record_coverage(state, &Coverage::arc_invalid_projection);
    }
    std::memcpy(slice, &constants, sizeof(constants));
}

void reap(State &state, ReleasedRecords &released)
{
    const auto completed = state.fence->GetCompletedValue();
    if (completed == UINT64_MAX)
    {
        state.counters.blocked = true;
        return;
    }
    unsigned count = 0;
    for (auto it = state.retired.begin(); it != state.retired.end();)
    {
        const auto &r = *it;
        const bool complete = !r->has_private_data ||
            (!r->quarantine && !r->arc_pending && r->last_fence != 0 && r->last_fence <= completed);
        if (r.use_count() != 1 || !complete)
        {
            ++it;
            continue;
        }
        for (const auto &draw : r->draws)
            if (draw.slice != no_slice)
                state.free_slices.push_back(draw.slice);
        released[count++] = std::move(*it);
        it = state.retired.erase(it);
    }
}

struct ArcRetirement
{
    ReleasedRecords records;
    unsigned count = 0;
    std::uint64_t fence = 0;
};

void begin_arc_retirement(State &state, ID3D12Resource *cleared_mask, std::uint64_t preceding_present,
                          ArcRetirement &retirement)
{
    if (cleared_mask == nullptr || state.counters.blocked)
        return;
    const auto append = [&](const Record &record) {
        // A wheel list in the same interval may have executed after the FSR
        // clear. Wait for a later clear before allowing its raw IDs to alias.
        if (record->arc_pending && !record->quarantine && record->last_fence != 0 &&
            record->last_fence <= preceding_present && record->arc_surface->mask() == cleared_mask)
            retirement.records[retirement.count++] = record;
    };
    for (const auto &[_, record] : state.active)
        append(record);
    for (const auto &record : state.retired)
        append(record);
    if (retirement.count == 0)
        return;
    if (state.next_fence == UINT64_MAX - 1)
    {
        state.counters.blocked = true;
        ++state.counters.arc_fence_failures;
        return;
    }
    state.submitting = true;
    retirement.fence = ++state.next_fence;
}

void finish_arc_retirement(State &state, ArcRetirement &retirement, bool signaled)
{
    for (unsigned i = 0; i < retirement.count; ++i)
    {
        auto &record = *retirement.records[i];
        if (signaled && !record.quarantine)
        {
            record.last_fence = retirement.fence;
            record.arc_pending = false;
        }
        else
            record.quarantine = true;
    }
    if (!signaled)
    {
        ++state.counters.arc_fence_failures;
        state.counters.blocked = true;
    }
    state.submitting = false;
}

struct Batch
{
    std::array<Record, max_recordings> records;
    std::array<bool, max_recordings> duplicates{};
    unsigned count = 0;
    bool reserved = false;
    std::uint64_t wait_fence = 0, signal_fence = 0;
};

void quarantine(State &state, const Batch &batch)
{
    for (unsigned i = 0; i < batch.count; ++i)
        batch.records[i]->quarantine = true;
    state.counters.blocked = true;
}

void begin_batch(State &state, ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists, Batch &batch)
{
    const std::lock_guard lock(state.mutex);
    if (!state.enabled || lists == nullptr)
        return;
    const bool primary = queue == state.queue &&
        (state.present_thread == 0 || state.present_thread == GetCurrentThreadId());
    bool private_data = false;
    for (UINT i = 0; i < count; ++i)
    {
        auto *command = lists[i];
        if (command == nullptr)
            continue;
        auto seen = state.exclusive_primary.find(command);
        if (seen == state.exclusive_primary.end() && state.exclusive_primary.size() < max_commands &&
            command->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
            seen = state.exclusive_primary.emplace(command, primary).first;
        if (!primary && seen != state.exclusive_primary.end())
            seen->second = false;
        const auto found = state.active.find(command);
        if (found == state.active.end())
            continue;
        const auto duplicate = std::find(batch.records.begin(), batch.records.begin() + batch.count, found->second);
        if (duplicate != batch.records.begin() + batch.count)
        {
            batch.duplicates[duplicate - batch.records.begin()] = true;
            continue;
        }
        batch.records[batch.count++] = found->second;
        private_data |= found->second->has_private_data;
    }
    if (batch.count == 0)
        return;
    if (!primary || state.submitting || state.counters.blocked)
    {
        ++state.counters.unsupported_submissions;
        for (unsigned i = 0; i < batch.count; ++i)
            batch.records[i]->quarantine = true;
        // A concurrent submission of an overridden list must see frozen bytes.
        // A foreign submission also has no trusted completion fence.
        if (private_data)
            quarantine(state, batch);
        return;
    }

    state.submitting = batch.reserved = true;
    for (unsigned i = 0; i < batch.count; ++i)
        if (batch.records[i]->has_private_data)
            batch.wait_fence = std::max(batch.wait_fence, batch.records[i]->last_fence);
}

bool wait_for_replay(const State &state, std::uint64_t value)
{
    if (value == 0)
        return true;
    auto completed = state.fence->GetCompletedValue();
    if (completed == UINT64_MAX)
        return false;
    if (completed >= value)
        return true;
    if (!ResetEvent(state.replay_event) || FAILED(state.fence->SetEventOnCompletion(value, state.replay_event)) ||
        WaitForSingleObject(state.replay_event, replay_wait_ms) != WAIT_OBJECT_0)
        return false;
    completed = state.fence->GetCompletedValue();
    return completed != UINT64_MAX && completed >= value;
}

void prepare_batch(State &state, Batch &batch)
{
    if (!batch.reserved)
        return;
    const bool ready = wait_for_replay(state, batch.wait_fence);
    const std::lock_guard lock(state.mutex);
    if (!ready || state.counters.blocked || state.next_fence == UINT64_MAX - 1)
    {
        if (!ready) ++state.counters.wait_failures;
        quarantine(state, batch);
        return;
    }

    bool private_data = false;
    for (unsigned i = 0; i < batch.count; ++i)
    {
        auto &record = *batch.records[i];
        if (record.quarantine)
            continue;
        const bool replay = record.submissions != 0 || batch.duplicates[i];

        // A first submission starts from the live CBV. Replay never contributes
        // history and writes stock data only after the last GPU read retires.
        if (replay && record.has_private_data)
            ++state.counters.replayed;
        const bool preserve_arc = replay && record.arc_pending;
        if (preserve_arc)
            ++state.counters.arc_replay_retained;
        for (auto &draw : record.draws)
            if (!preserve_arc || !draw.arc_enabled)
                fill_before_submit(state, draw, replay);
        if (record.arc_surface)
            record.arc_pending = true;
        if (record.submissions < UINT_MAX) ++record.submissions;
        private_data |= record.has_private_data;
    }
    if (private_data)
    {
        batch.signal_fence = ++state.next_fence;
        for (unsigned i = 0; i < batch.count; ++i)
            if (batch.records[i]->has_private_data && !batch.records[i]->quarantine)
                batch.records[i]->last_fence = batch.signal_fence;
    }
}

void finish_batch(State &state, const Batch &batch, bool signaled)
{
    if (!batch.reserved)
        return;
    const std::lock_guard lock(state.mutex);
    if (!signaled)
        quarantine(state, batch);
    state.submitting = false;
}

thread_local bool inside_execute = false;

struct ExecuteGuard
{
    ExecuteGuard() { inside_execute = true; }
    ~ExecuteGuard() { inside_execute = false; }
};

void STDMETHODCALLTYPE execute(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists) noexcept
{
    const auto original = process().original.load(std::memory_order_acquire);
    if (inside_execute)
    {
        original(queue, count, lists);
        return;
    }

    const ExecuteGuard guard;
    std::shared_ptr<State> state;
    Batch batch;
    try
    {
        state = current_state();
        if (state)
        {
            begin_batch(*state, queue, count, lists, batch);
            prepare_batch(*state, batch);
        }
    }
    catch (...)
    {
        // An observation failure must still submit the game's original lists.
        // Retain all potentially affected uploads rather than guess at lifetime.
        if (state)
        {
            const std::lock_guard lock(state->mutex);
            quarantine(*state, batch);
            for (auto &[_, record] : state->active)
                record->quarantine = true;
        }
    }
    original(queue, count, lists);
    const bool signaled = batch.signal_fence == 0 || SUCCEEDED(queue->Signal(state->fence.Get(), batch.signal_fence));
    if (state)
        finish_batch(*state, batch, signaled);
}

bool install_queue_hook(Process &p, ID3D12CommandQueue *queue)
{
    // The public ID3D12CommandQueue vtable places ExecuteCommandLists at 10.
    void *target = (*reinterpret_cast<void *const *const *>(queue))[10];
    if (p.target != nullptr)
        return p.target == target;
    HMODULE module = nullptr;
    if (target == nullptr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(target), &module))
        return false;
    void *trampoline = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void *>(execute), &trampoline) != MH_OK)
    {
        FreeLibrary(module);
        return false;
    }
    p.original.store(reinterpret_cast<Execute>(trampoline), std::memory_order_release);
    if (MH_EnableHook(target) != MH_OK)
    {
        MH_RemoveHook(target);
        p.original.store(nullptr, std::memory_order_relaxed);
        FreeLibrary(module);
        return false;
    }
    p.target = target;
    p.module = module;
    return true;
}

bool make_pool(State &state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = pool_size;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(state.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&state.pool))) ||
        FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.fence))))
        return false;
    void *mapping = nullptr;
    constexpr D3D12_RANGE no_reads{};
    if (FAILED(state.pool->Map(0, &no_reads, &mapping)) || mapping == nullptr)
        return false;
    state.mapping = static_cast<std::byte *>(mapping);
    state.pool_address = state.pool->GetGPUVirtualAddress();
    state.replay_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (state.pool_address == 0 || state.replay_event == nullptr)
        return false;
    state.pool->SetName(L"GTA V Native Wheel Motion Constants");
    state.free_slices.reserve(slice_count);
    for (unsigned i = slice_count; i != 0; --i)
        state.free_slices.push_back(i - 1);
    state.retired.reserve(max_recordings);
    state.interval.reserve(max_history);
    state.previous.reserve(max_history);
    state.interval_claims.reserve(max_history);
    state.arc_identities.reserve(max_history);
    return true;
}

void replace_recording(const std::shared_ptr<State> &state, ID3D12CommandList *command, bool destroyed)
{
    ReleasedRecords released;
    const std::lock_guard lock(state->mutex);
    if (destroyed)
        state->exclusive_primary.erase(command);
    if (const auto found = state->active.find(command); found != state->active.end())
    {
        auto record = std::move(found->second);
        state->active.erase(found);
        if (record->has_private_data && record->last_fence == 0 && record.use_count() == 1)
            record->quarantine = true; // Never observed a submission for this recording.
        state->retired.push_back(std::move(record));
    }
    reap(*state, released);
}
} // namespace

bool init(ID3D12Device *device, ID3D12CommandQueue *queue) noexcept
{
    if (device == nullptr || queue == nullptr || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return false;
    try
    {
        auto &p = process();
        const std::lock_guard init_lock(p.initialize);
        if (const auto active = current_state())
            return active->device == device && active->queue == queue;

        auto state = std::make_shared<State>();
        state->device = device;
        state->queue = queue;
        if (!make_pool(*state) || !install_queue_hook(p, queue))
            return false;
        const std::lock_guard lock(p.state_mutex);
        if (p.pending_device == device)
        {
            state->uploads = std::move(p.pending_uploads);
            p.pending_device = nullptr;
        }
        p.active = std::move(state);
        return true;
    }
    catch (...) { return false; }
}

void register_upload(ID3D12Device *device, ID3D12Resource *resource, std::uint64_t size,
                     D3D12_GPU_VIRTUAL_ADDRESS address) noexcept
{
    if (device == nullptr || resource == nullptr || address == 0 || size < sizeof(Constants))
        return;
    const auto desc = resource->GetDesc();
    D3D12_HEAP_PROPERTIES heap{};
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || size != desc.Width || address != resource->GetGPUVirtualAddress() ||
        FAILED(resource->GetHeapProperties(&heap, nullptr)) ||
        (heap.Type != D3D12_HEAP_TYPE_UPLOAD && heap.Type != gpu_upload_heap))
        return;
    try
    {
        auto &p = process();
        std::shared_ptr<State> active;
        {
            const std::lock_guard lock(p.state_mutex);
            active = p.active;
            if (!active)
            {
                if (p.pending_device == nullptr) p.pending_device = device;
                if (p.pending_device == device && p.pending_uploads.size() < max_uploads)
                    p.pending_uploads.insert_or_assign(resource, Upload{address, size});
                return;
            }
        }
        if (active->device == device)
        {
            const std::lock_guard lock(active->mutex);
            if (active->uploads.size() < max_uploads)
                active->uploads.insert_or_assign(resource, Upload{address, size});
        }
    }
    catch (...) { /* Preserve native resource creation. */ }
}

void unregister_upload(ID3D12Device *device, ID3D12Resource *resource) noexcept
{
    try
    {
        auto &p = process();
        std::shared_ptr<State> active;
        {
            const std::lock_guard lock(p.state_mutex);
            if (p.pending_device == device)
                p.pending_uploads.erase(resource);
            active = p.active;
        }
        if (active && active->device == device)
        {
            const std::lock_guard lock(active->mutex);
            active->uploads.erase(resource);
        }
    }
    catch (...) { /* Preserve native resource destruction. */ }
}

bool try_draw(ID3D12Device *device, ID3D12GraphicsCommandList *command,
              const wheel_override::GraphicsBindings &bindings, D3D12_CPU_DESCRIPTOR_HANDLE cbv,
              Geometry geometry, const wheel_override::IndexedDraw &indexed_draw, const D3D12_VIEWPORT &viewport,
              ID3D12Resource *motion_rt, const wheel_override::OutputBindings &outputs) noexcept
{
    if (device == nullptr)
        return false;

    try
    {
        const auto state = state_for(device);
        if (!state)
            return false;
        state->attempted.fetch_add(1, std::memory_order_relaxed);
        if (command == nullptr || cbv.ptr == 0 || geometry.index_buffer == nullptr || indexed_draw.index_count == 0 ||
            indexed_draw.instance_count != 1 || motion_rt == nullptr ||
            !(viewport.Width >= 1 && viewport.Width <= 8192 && viewport.Height >= 1 && viewport.Height <= 8192) ||
            viewport.TopLeftX != 0 || viewport.TopLeftY != 0 || command->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return false;
        const auto width = static_cast<std::uint32_t>(viewport.Width), height = static_cast<std::uint32_t>(viewport.Height);
        if (static_cast<float>(width) != viewport.Width || static_cast<float>(height) != viewport.Height)
            return false;
        const auto match = wheel_pipeline_hooks::find(bindings.pipeline);
        if (!match || match->original_signature.Get() != bindings.root_signature ||
            std::any_of(bindings.tables.begin(), bindings.tables.end(), [](const auto &table) { return table.ptr == 0; }))
            return false;

        auto surface = outputs.valid && outputs.scene_depth != nullptr
            ? wheel_mask::find_surface(device, motion_rt, outputs.scene_depth) : nullptr;
        if (surface && (!surface->initialized() || width > surface->width() || height > surface->height() ||
            !match->replacement.supports_mask(surface->reverse_z())))
            surface.reset();

        auto *identity = static_cast<ID3D12CommandList *>(command);
        Draw pending;
        pending.mesh = {geometry.index_buffer, geometry.index_offset, indexed_draw.index_count, indexed_draw.first_index,
            indexed_draw.base_vertex, indexed_draw.first_instance};
        pending.target = {motion_rt, viewport.TopLeftX, viewport.TopLeftY, width, height};
        pending.cbv = cbv;

        const std::lock_guard lock(state->mutex);
        ++state->counters.pipeline_matches;
        const auto seen = state->exclusive_primary.find(identity);
        if (state->counters.blocked || seen == state->exclusive_primary.end() || !seen->second)
            return false;
        ++state->counters.queue_admitted;
        if (!resolve_source(*state, pending))
            return false;
        ++state->counters.source_resolved;

        auto recording = state->active.find(identity);
        if (recording == state->active.end())
        {
            if (state->active.size() + state->retired.size() == max_recordings)
            {
                ++state->counters.dropped;
                return false;
            }
            recording = state->active.try_emplace(identity, std::make_shared<Recording>()).first;
        }
        auto &record = *recording->second;
        if (record.draws.size() == max_draws || record.submissions != 0 || record.quarantine)
        {
            ++state->counters.dropped;
            return false;
        }

        Constants initial;
        if (!recently_verified_target(*state, pending.target))
            pending.decision = RecordDecision::no_history;
        else if (state->free_slices.empty())
            pending.decision = RecordDecision::pool_full;
        else
        {
            // The previous frame can present and the game can finalize the
            // upload after recording. Establish identity at Execute using
            // that submitted history and the live stock CBV.
            read_constants(pending, initial);
            const bool finite_seed = finite(initial.world) && finite(initial.current) && finite(initial.previous);
            pending.decision = finite_seed ? RecordDecision::override_failed : RecordDecision::unsafe_seed;
        }
        auto &saved = record.draws.emplace_back(std::move(pending));
        ++state->counters.observed;
        if (saved.decision != RecordDecision::override_failed)
            return false; // Still observe the stock draw at its actual submission.

        const auto slice = state->free_slices.back();
        auto *allocation = state->mapping + slice * slice_size;
        // A replay through an unobserved path still sees stock bytes. The real
        // primary queue hook refreshes them before any first observed use.
        std::memcpy(allocation, &initial, sizeof(initial));
        std::memset(allocation + sizeof(initial), 0, slice_size - sizeof(initial));
        const auto address = state->pool_address + slice * slice_size;
        std::optional<wheel_override::MaskDraw> mask;
        if (surface && surface->initialized() && viewport.MinDepth == 0 && viewport.MaxDepth == 1 &&
            matches_frame(saved.target, state->frame) &&
            (!record.arc_surface || record.arc_surface == surface))
            mask.emplace(wheel_override::MaskDraw{outputs, *surface, slice + 1});
        if (!match->replacement.draw_indexed(command, bindings, address, indexed_draw, mask ? &*mask : nullptr))
            return false;

        // No allocation or exception-prone work after recording the draw.
        state->free_slices.pop_back();
        record.has_private_data = true;
        saved.slice = slice;
        saved.decision = RecordDecision::substituted;
        saved.pipeline = match;
        if (mask)
        {
            record.arc_surface = std::move(surface);
            saved.arc_enabled = true;
            ++state->counters.arc_recorded;
        }
        ++state->counters.substituted;
        return true;
    }
    catch (...) { return false; }
}

ArcPool arc_pool(ID3D12Device *device) noexcept
{
    try
    {
        if (const auto state = state_for(device))
            return {state->pool.Get(), slice_size, slice_count};
    }
    catch (...) { /* The caller can still use linear blur. */ }
    return {};
}

void reset(ID3D12Device *device, ID3D12GraphicsCommandList *command) noexcept
{
    try
    {
        if (const auto state = state_for(device); state && command != nullptr)
            replace_recording(state, command, false);
    }
    catch (...) { /* Keep the old allocation if recording replacement is uncertain. */ }
}

void destroy_command(ID3D12Device *device, ID3D12GraphicsCommandList *command) noexcept
{
    try
    {
        if (const auto state = state_for(device); state && command != nullptr)
            replace_recording(state, command, true);
    }
    catch (...) { /* Keep the old allocation if recording replacement is uncertain. */ }
}

void present(ID3D12Device *device, const Frame &frame) noexcept
{
    try
    {
        const auto state = state_for(device);
        if (!state)
            return;
        ReleasedRecords released;
        ArcRetirement retirement;
        {
            const std::lock_guard lock(state->mutex);
            const auto thread = GetCurrentThreadId();
            if (state->present_thread != 0 && state->present_thread != thread)
                state->counters.blocked = true;
            state->present_thread = thread;
            if (state->submitting)
                state->counters.blocked = true;

            const bool contiguous = !state->have_frame ||
                (state->frame.frame_id != UINT64_MAX && frame.frame_id == state->frame.frame_id + 1);
            const bool stable_view = !valid_frame(state->frame) || (state->frame.context_id == frame.context_id &&
                state->frame.render_width == frame.render_width && state->frame.render_height == frame.render_height &&
                state->frame.upscale_width == frame.upscale_width && state->frame.upscale_height == frame.upscale_height);
            const bool sequential_dispatches = !valid_frame(state->frame) ||
                (state->frame.dispatch_id != UINT64_MAX && frame.dispatch_id == state->frame.dispatch_id + 1);
            const bool sequential = contiguous && stable_view && sequential_dispatches;
            const bool frame_valid = valid_frame(frame);
            if (frame_valid)
            {
                state->verified_target = {frame.motion_rt, 0, 0, frame.render_width, frame.render_height};
                state->verified_target_frame = frame.frame_id;
            }
            state->interval_claims.clear();
            state->arc_identities.clear();
            state->previous.clear();
            if (sequential && frame_valid && !state->interval_overflow && !state->counters.blocked)
            {
                std::erase_if(state->interval, [&](const Observation &o) { return !matches_frame(o.target, frame); });
                state->previous.swap(state->interval);
            }
            state->interval.clear();
            state->interval_overflow = false;
            state->recent_coverage[frame.frame_id % coverage_history] = FrameCoverage{
                frame.frame_id, frame.dispatch_id, state->interval_coverage,
                static_cast<std::uint32_t>(state->previous.size()), frame_valid, sequential};
            state->interval_coverage = {};
            state->frame = frame;
            state->frame.cleared_arc_mask = nullptr;
            state->have_frame = true;
            const auto preceding_present = std::exchange(state->present_fence_cutoff, state->next_fence);
            begin_arc_retirement(*state, frame.cleared_arc_mask, preceding_present, retirement);
        }

        const bool signaled = retirement.fence == 0 || SUCCEEDED(state->queue->Signal(state->fence.Get(), retirement.fence));
        const std::lock_guard lock(state->mutex);
        if (retirement.fence != 0)
        {
            finish_arc_retirement(*state, retirement, signaled);
            for (unsigned i = 0; i < retirement.count; ++i)
                retirement.records[i].reset();
        }
        reap(*state, released);
    }
    catch (...) { /* A failed diagnostic cannot affect presentation. */ }
}

void invalidate_history(ID3D12Device *device) noexcept
{
    try
    {
        if (const auto state = state_for(device))
        {
            const std::lock_guard lock(state->mutex);
            state->interval_claims.clear();
            state->arc_identities.clear();
            state->previous.clear();
            state->interval.clear();
            state->interval_coverage = {};
            state->interval_overflow = false;
            state->frame = {};
            state->verified_target = {};
            state->verified_target_frame = 0;
            state->have_frame = false;
        }
    }
    catch (...) { /* Preserve the native lifecycle callback. */ }
}

Stats stats(ID3D12Device *device) noexcept
{
    try
    {
        if (const auto state = state_for(device))
        {
            const std::lock_guard lock(state->mutex);
            auto counters = state->counters;
            counters.attempted = state->attempted.load(std::memory_order_relaxed);
            counters.upload_bytes_used = (slice_count - state->free_slices.size()) * slice_size;
            return counters;
        }
    }
    catch (...) {}
    return {};
}

std::optional<FrameCoverage> coverage_for_frame(ID3D12Device *device, std::uint64_t frame_id) noexcept
{
    try
    {
        if (const auto state = state_for(device))
        {
            const std::lock_guard lock(state->mutex);
            const auto &entry = state->recent_coverage[frame_id % coverage_history];
            if (entry && entry->frame_id == frame_id)
                return entry;
        }
    }
    catch (...) { /* The capture may proceed without coverage diagnostics. */ }
    return std::nullopt;
}

void shutdown_device(ID3D12Device *device) noexcept
{
    try
    {
        std::shared_ptr<State> retired;
        auto &p = process();
        const std::lock_guard init_lock(p.initialize);
        {
            const std::lock_guard lock(p.state_mutex);
            if (p.active && p.active->device == device)
                retired = std::move(p.active);
            if (p.pending_device == device)
            {
                p.pending_uploads.clear();
                p.pending_device = nullptr;
            }
        }
        if (retired)
        {
            const std::lock_guard lock(retired->mutex);
            retired->enabled = false;
        }
    }
    catch (...) { /* Keep any uncertain GPU lifetime alive until process exit. */ }
}
} // namespace wheel_runtime
