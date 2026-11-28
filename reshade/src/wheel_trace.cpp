#include <windows.h>
#include <d3d12.h>
#include <MinHook.h>
#include <reshade.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "fsr_probe.hpp"
#include "wheel_mask.hpp"
#include "wheel_pipeline_hooks.hpp"
#include "wheel_runtime.hpp"
#include "wheel_trace.hpp"

namespace wheel_trace
{
namespace
{
namespace api = reshade::api;
constexpr std::uint32_t wheel_root_parameter = 2;
constexpr std::uint32_t wheel_descriptor_offset = 7;
constexpr unsigned draw_limit = 48;
constexpr unsigned copy_limit = 128;
constexpr unsigned wheel_depth_limit = 4;
constexpr unsigned full_copy_limit = 32;
constexpr unsigned descriptor_limit = 131072;
constexpr unsigned buffer_limit = 8192;
constexpr unsigned view_limit = 8192;
constexpr unsigned command_limit = 4096;
constexpr std::uint64_t capture_timeout_ms = 15000;
// The cross-compiler's D3D12 headers predate this SDK enum value.
constexpr auto gpu_upload_heap = static_cast<D3D12_HEAP_TYPE>(5);
using Matrices = std::array<float, 48>;
static_assert(sizeof(Matrices) == 192);

struct ShaderId
{
    std::size_t bytes;
    std::uint32_t crc;
};

// Steam build 25971420. See docs/enhanced-wheel-motion.md for the reflection
// and DXIL proof. Filter by size first to avoid hashing unrelated shader blobs.
constexpr std::array wheel_vs{
    ShaderId{6136, 0xDF51595B}, ShaderId{6160, 0x2C667625},
    ShaderId{6576, 0x21145BC8}, ShaderId{6600, 0x2E344675},
};
constexpr std::array wheel_ps{
    ShaderId{7188, 0xDEF27DF8}, ShaderId{8164, 0x5DE20624}, ShaderId{7300, 0x91F4F5B9},
    ShaderId{7164, 0x00FAFC23}, ShaderId{8140, 0x6DC98463}, ShaderId{7276, 0x65CD9D99},
    ShaderId{7220, 0x1B5B11EC}, ShaderId{8192, 0x2A35207B}, ShaderId{7328, 0xDB4E62ED},
    ShaderId{7196, 0x48718FC0}, ShaderId{8168, 0xF856AEF8}, ShaderId{7304, 0xD3147194},
};

constexpr auto crc_table = [] {
    std::array<std::uint32_t, 256> result{};
    for (unsigned i = 0; i < result.size(); ++i)
    {
        std::uint32_t remainder = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            remainder = (remainder >> 1) ^ ((remainder & 1) != 0 ? 0xEDB88320u : 0);
        result[i] = remainder;
    }
    return result;
}();

template <std::size_t N>
std::uint32_t recognize(const api::shader_desc &shader, const std::array<ShaderId, N> &known)
{
    if (shader.code == nullptr || std::none_of(known.begin(), known.end(), [&](const ShaderId &entry) {
        return entry.bytes == shader.code_size;
    }))
        return 0;
    std::uint32_t crc = UINT32_MAX;
    const auto *bytes = static_cast<const unsigned char *>(shader.code);
    for (std::size_t i = 0; i < shader.code_size; ++i)
        crc = (crc >> 8) ^ crc_table[(crc ^ bytes[i]) & 0xffu];
    crc = ~crc;
    return std::any_of(known.begin(), known.end(), [&](const ShaderId &entry) {
        return entry.bytes == shader.code_size && entry.crc == crc;
    }) ? crc : 0;
}

struct Pipeline
{
    std::uint32_t vs = 0, ps = 0;
    api::pipeline_layout layout{};

    bool rigid_tire() const { return vs == wheel_vs[0].crc || vs == wheel_vs[1].crc; }
};

struct Recording
{
    Pipeline pipeline{};
    api::pipeline native_pipeline{};
    api::pipeline_layout graphics_layout{};
    std::array<api::descriptor_table, 3> graphics_tables{};
    api::resource rt4{}, depth{};
    api::resource_view rt4_view{}, depth_view{};
    std::uint32_t mip = 0, layer = 0;
    api::resource index_buffer{};
    std::uint64_t index_offset = 0;
    api::viewport viewport{};
    bool single_viewport = false, native_targets_bound = false;
    wheel_override::OutputBindings outputs{};
};

struct NativeOutputState
{
    struct Query
    {
        ID3D12QueryHeap *heap;
        D3D12_QUERY_TYPE type;
        UINT index;

        bool operator==(const Query &) const = default;
    };

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
    std::array<float, 4> blend{1, 1, 1, 1};
    std::array<api::resource_view, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> pending_targets{};
    api::resource_view pending_depth{};
    UINT pending_count = 0;
    std::array<Query, 16> active_queries{};
    unsigned active_query_count = 0;
    bool query_tracking_failed = false;
    bool tracked = false, blend_known = false, in_pass = false, targets_observed = false;

    bool query_active() const noexcept { return active_query_count != 0 || query_tracking_failed; }
};

struct Buffer
{
    std::uint64_t size, lifetime;
    api::memory_heap heap;
};

struct ConstantBuffer
{
    api::buffer_range range{};
    std::uint64_t lifetime = 0;
};

struct TargetView
{
    api::resource resource{};
    std::uint32_t mip = 0, layer = 0;
    bool depth_stencil = false;
};

using DescriptorKey = std::pair<std::uint64_t, std::uint32_t>;

struct DescriptorEntry
{
    ConstantBuffer cbv{};
    std::list<DescriptorKey>::iterator recency;
};

using DescriptorMap = std::map<DescriptorKey, DescriptorEntry>;

struct Draw
{
    Pipeline pipeline{};
    std::uint64_t command = 0, table = 0, index_buffer = 0, index_offset = 0;
    api::resource rt4{}, depth{};
    api::resource_view rt4_view{};
    api::viewport viewport{};
    std::uint32_t mip = 0, layer = 0;
    std::uint32_t indices = 0, instances = 0, first_index = 0, first_instance = 0;
    std::int32_t base_vertex = 0;
    ConstantBuffer cbv{};
    Matrices matrices{};
    bool cpu_snapshot = false;
    bool indirect = false;
    bool indexed = false;
    bool layout_verified = false;
    bool mask_snapshot = false, native_targets_bound = false, native_mask_tracking = false;
    bool native_render_pass = false, native_query = false;
};

struct DrawReplacement
{
    ID3D12Device *device = nullptr;
    ID3D12GraphicsCommandList *command = nullptr;
    wheel_override::GraphicsBindings bindings{};
    D3D12_CPU_DESCRIPTOR_HANDLE cbv{};
    wheel_runtime::Geometry geometry{};
    D3D12_VIEWPORT viewport{};
    ID3D12Resource *motion_rt = nullptr;
    wheel_override::OutputBindings outputs{};
};

struct DepthCopy
{
    std::uint64_t command = 0;
    api::resource source{}, destination{};
    std::uint32_t source_subresource = 0, destination_subresource = 0;
    api::subresource_box source_box{}, destination_box{};
    bool texture_region = false, has_source_box = false, has_destination_box = false;
};

struct WheelDepthTarget
{
    Microsoft::WRL::ComPtr<ID3D12Resource> motion, depth;
    D3D12_RESOURCE_DESC description{};
};

struct FullDepthCopy
{
    std::uint64_t source, destination;
    bool operator==(const FullDepthCopy &) const = default;
};

struct DepthAssociation
{
    std::array<WheelDepthTarget, wheel_depth_limit> targets{};
    std::array<FullDepthCopy, full_copy_limit> full_copies{};
    std::array<std::uintptr_t, 2> observed_fsr_depth{};
    std::uintptr_t context = 0, motion = 0;
    UINT width = 0, height = 0;
    unsigned targets_seen = 0, copies_seen = 0;
    bool targets_overflowed = false, copies_overflowed = false;
};

struct Device
{
    Device() = default;
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    std::unordered_map<std::uint64_t, Pipeline> pipelines;
    std::unordered_set<std::uint64_t> wheel_layouts;
    std::unordered_map<std::uint64_t, Recording> commands;
    std::unordered_map<std::uint64_t, NativeOutputState> reset_tracked_commands;
    std::unordered_map<std::uint64_t, Buffer> buffers;
    std::unordered_map<std::uint64_t, TargetView> views;
    std::unordered_map<std::uint64_t, unsigned> view_references;
    std::list<DescriptorKey> descriptor_recency;
    DescriptorMap descriptors;
    std::vector<std::pair<DescriptorKey, ConstantBuffer>> copy_snapshot;
    std::array<Draw, draw_limit> draws{};
    std::array<DepthCopy, copy_limit> copies{};
    std::uintptr_t watched_fsr_depth = 0;
    unsigned copies_recorded = 0, copies_left = 0, copies_omitted = 0;
    DepthAssociation depth_association;
    std::uint64_t next_lifetime = 0, armed_tick = 0;
    std::uint64_t buffer_overflows = 0, view_overflows = 0, descriptor_overflows = 0;
    std::uint64_t descriptor_evictions = 0;
    unsigned remaining = 0, recorded = 0, omitted = 0;
    UINT rtv_stride = 0;
    bool correction_active = false;
};

std::mutex trace_mutex;
std::unordered_map<api::device *, Device> devices;
std::once_flag register_once;
std::atomic_bool burst_active{false};
std::atomic_bool any_correction_active{false};
bool trace_matrices = false;

using SetBlend = void (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, const float *);
using SetTargets = void (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *,
                                            BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE *);
using ClearState = void (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12PipelineState *);
using Query = void (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT);
using BeginPass = void (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList4 *, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *,
                                            const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *, D3D12_RENDER_PASS_FLAGS);
using EndPass = void (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList4 *);

template <typename Function>
struct NativeHook
{
    std::atomic<Function> original{};
    void *target = nullptr;
    void *failed_target = nullptr;
    HMODULE module = nullptr;
};

std::mutex install_mutex;
NativeHook<SetBlend> blend_hook;
NativeHook<SetTargets> targets_hook;
NativeHook<ClearState> clear_hook;
NativeHook<Query> begin_query_hook, end_query_hook;
NativeHook<BeginPass> begin_hook;
NativeHook<EndPass> end_hook;
thread_local bool recording_wheel = false;

template <typename Callback>
void native_outputs(void *command, Callback &&callback) noexcept
{
    if (recording_wheel)
        return;
    try
    {
        const auto key = reinterpret_cast<std::uint64_t>(command);
        const std::lock_guard lock(trace_mutex);
        for (auto &[_, state] : devices)
            if (auto entry = state.reset_tracked_commands.find(key); entry != state.reset_tracked_commands.end())
            {
                if (entry->second.tracked)
                    callback(state, entry->second, key);
                return;
            }
    }
    catch (...) {}
}

void invalidate_outputs(Device &device, std::uint64_t command)
{
    if (auto entry = device.commands.find(command); entry != device.commands.end())
    {
        entry->second.outputs = {};
        entry->second.native_targets_bound = false;
    }
}

void STDMETHODCALLTYPE native_set_blend(ID3D12GraphicsCommandList *command, const float *factor)
{
    blend_hook.original.load(std::memory_order_acquire)(command, factor);
    native_outputs(command, [&](Device &, NativeOutputState &state, std::uint64_t) {
        if (factor != nullptr)
            std::copy_n(factor, state.blend.size(), state.blend.begin());
        else
            state.blend.fill(1.0f);
        state.blend_known = true;
    });
}

void STDMETHODCALLTYPE native_set_targets(ID3D12GraphicsCommandList *command, UINT count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *targets, BOOL contiguous, const D3D12_CPU_DESCRIPTOR_HANDLE *depth)
{
    targets_hook.original.load(std::memory_order_acquire)(command, count, targets, contiguous, depth);
    if (any_correction_active.load(std::memory_order_relaxed))
        native_outputs(command, [&](Device &device, NativeOutputState &state, std::uint64_t key) {
            invalidate_outputs(device, key);
            state.targets_observed = targets != nullptr && count <= state.pending_targets.size() && device.rtv_stride != 0;
            if (!state.targets_observed)
                return;
            state.pending_count = count;
            state.pending_depth = {depth != nullptr ? depth->ptr : 0};
            for (UINT i = 0; i < count; ++i)
                state.pending_targets[i] = {contiguous ? targets[0].ptr + static_cast<SIZE_T>(i) * device.rtv_stride : targets[i].ptr};
        });
}

void STDMETHODCALLTYPE native_clear_state(ID3D12GraphicsCommandList *command, ID3D12PipelineState *pipeline)
{
    clear_hook.original.load(std::memory_order_acquire)(command, pipeline);
    native_outputs(command, [&](Device &device, NativeOutputState &state, std::uint64_t key) {
        state.blend_known = state.targets_observed = false;
        invalidate_outputs(device, key);
    });
}

void STDMETHODCALLTYPE native_begin_query(ID3D12GraphicsCommandList *command, ID3D12QueryHeap *heap,
                                           D3D12_QUERY_TYPE type, UINT index)
{
    begin_query_hook.original.load(std::memory_order_acquire)(command, heap, type, index);
    if (type == D3D12_QUERY_TYPE_TIMESTAMP)
        return;
    native_outputs(command, [&](Device &, NativeOutputState &state, std::uint64_t) {
        if (state.query_tracking_failed)
            return;
        const NativeOutputState::Query query{heap, type, index};
        const auto end = state.active_queries.begin() + state.active_query_count;
        if (end == state.active_queries.end() || std::find(state.active_queries.begin(), end, query) != end)
            state.query_tracking_failed = true;
        else
            state.active_queries[state.active_query_count++] = query;
    });
}

void STDMETHODCALLTYPE native_end_query(ID3D12GraphicsCommandList *command, ID3D12QueryHeap *heap,
                                         D3D12_QUERY_TYPE type, UINT index)
{
    end_query_hook.original.load(std::memory_order_acquire)(command, heap, type, index);
    if (type == D3D12_QUERY_TYPE_TIMESTAMP)
        return;
    native_outputs(command, [&](Device &, NativeOutputState &state, std::uint64_t) {
        if (state.query_tracking_failed)
            return;
        const auto end = state.active_queries.begin() + state.active_query_count;
        const auto query = std::find(state.active_queries.begin(), end, NativeOutputState::Query{heap, type, index});
        if (query == end)
            state.query_tracking_failed = true;
        else
            *query = state.active_queries[--state.active_query_count];
    });
}

void STDMETHODCALLTYPE native_begin_pass(ID3D12GraphicsCommandList4 *command, UINT count,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC *targets, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth,
    D3D12_RENDER_PASS_FLAGS flags)
{
    begin_hook.original.load(std::memory_order_acquire)(command, count, targets, depth, flags);
    native_outputs(command, [&](Device &device, NativeOutputState &state, std::uint64_t key) {
        state.in_pass = true;
        state.targets_observed = false;
        invalidate_outputs(device, key);
    });
}

void STDMETHODCALLTYPE native_end_pass(ID3D12GraphicsCommandList4 *command)
{
    end_hook.original.load(std::memory_order_acquire)(command);
    native_outputs(command, [&](Device &device, NativeOutputState &state, std::uint64_t key) {
        state.in_pass = state.targets_observed = false;
        invalidate_outputs(device, key);
    });
}

template <typename Function>
bool install_hook(NativeHook<Function> &hook, void *target, Function detour)
{
    if (hook.target != nullptr)
        return hook.target == target;
    if (target == nullptr || target == hook.failed_target)
        return false;
    hook.failed_target = target;
    LPCWSTR owner = reinterpret_cast<LPCWSTR>(target);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, owner, &hook.module))
        return false;
    void *original = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void *>(detour), &original) == MH_OK)
    {
        hook.original.store(reinterpret_cast<Function>(original), std::memory_order_release);
        if (MH_EnableHook(target) == MH_OK)
        {
            hook.target = target;
            return true;
        }
        MH_RemoveHook(target);
        hook.original.store(nullptr, std::memory_order_release);
    }
    FreeLibrary(hook.module);
    hook.module = nullptr;
    return false;
}

bool track_native_outputs(ID3D12GraphicsCommandList *command)
{
    if (command->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return false;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> v4;
    const auto queried = command->QueryInterface(IID_PPV_ARGS(&v4));
    if (queried != E_NOINTERFACE && (FAILED(queried) || static_cast<ID3D12GraphicsCommandList *>(v4.Get()) != command))
        return false;

    const std::lock_guard lock(install_mutex);
    auto *vtable = *reinterpret_cast<void ***>(command);
    // COM vtable slots from the SDK interfaces. Require all the installed
    // slots to match this command list before relying on its exact OM state.
    return install_hook(clear_hook, vtable[11], native_clear_state) &&
        install_hook(blend_hook, vtable[23], native_set_blend) &&
        install_hook(targets_hook, vtable[46], native_set_targets) &&
        install_hook(begin_query_hook, vtable[52], native_begin_query) &&
        install_hook(end_query_hook, vtable[53], native_end_query) &&
        (v4 == nullptr || (install_hook(begin_hook, vtable[68], native_begin_pass) &&
                           install_hook(end_hook, vtable[69], native_end_pass)));
}

template <typename Callback>
void inspect(api::device *device, Callback &&callback) noexcept
{
    try
    {
        const std::lock_guard lock(trace_mutex);
        if (const auto found = devices.find(device); found != devices.end())
            callback(found->second);
    }
    catch (...) { /* A failed observation cannot interfere with the game call. */ }
}

void update_active()
{
    burst_active.store(std::any_of(devices.begin(), devices.end(), [](const auto &entry) {
        return entry.second.remaining != 0;
    }), std::memory_order_relaxed);
    any_correction_active.store(std::any_of(devices.begin(), devices.end(), [](const auto &entry) {
        return entry.second.correction_active;
    }), std::memory_order_relaxed);
}

void write_log(const char *message)
{
    reshade::log::message(reshade::log::level::info, message);
}

bool marker_exists(const wchar_t *marker) noexcept
{
    wchar_t path[32768]{};
    const auto size = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (size == 0 || size == std::size(path))
        return false;
    auto *separator = std::wcsrchr(path, L'\\');
    const auto marker_length = std::wcslen(marker) + 1;
    if (separator == nullptr || static_cast<std::size_t>(separator - path) + marker_length >= std::size(path))
        return false;
    std::memcpy(separator + 1, marker, marker_length * sizeof(wchar_t));
    const auto attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool trace_requested() noexcept
{
    static const bool active = [] {
        wchar_t value[2]{};
        return (GetEnvironmentVariableW(L"GTA_NATIVE_WHEEL_TRACE", value, 2) == 1 && value[0] == L'1') ||
            marker_exists(L"NativeWheelTrace.enable");
    }();
    return active;
}

bool descriptor_key(api::device *device, api::descriptor_table table, std::uint32_t binding,
                    DescriptorKey &key)
{
    if (table.handle == 0)
        return false;
    api::descriptor_heap heap{};
    std::uint32_t base = 0;
    device->get_descriptor_heap_offset(table, 0, 0, &heap, &base);
    if (heap.handle == 0 || binding > UINT32_MAX - base)
        return false;
    key = {heap.handle, base + binding};
    return true;
}

DescriptorMap::iterator erase_descriptor(Device &state, DescriptorMap::iterator entry)
{
    state.descriptor_recency.erase(entry->second.recency);
    return state.descriptors.erase(entry);
}

bool inside_descriptor_range(const DescriptorKey &key, const DescriptorKey &start, std::uint32_t count)
{
    return key.first == start.first && key.second >= start.second && key.second - start.second < count;
}

void touch_descriptor(Device &state, const DescriptorEntry &entry)
{
    state.descriptor_recency.splice(state.descriptor_recency.end(), state.descriptor_recency, entry.recency);
}

void remember_descriptor(Device &state, const DescriptorKey &key, const ConstantBuffer &cbv)
{
    if (const auto found = state.descriptors.find(key); found != state.descriptors.end())
    {
        found->second.cbv = cbv;
        touch_descriptor(state, found->second);
        return;
    }

    if (state.descriptors.size() == descriptor_limit)
    {
        // Reuse both nodes when the cache is full. Active descriptor tables
        // churn every frame, so an eviction must not require a new allocation.
        const auto oldest = state.descriptor_recency.begin();
        auto entry = state.descriptors.extract(*oldest);
        *oldest = key;
        entry.key() = key;
        entry.mapped().cbv = cbv;
        touch_descriptor(state, entry.mapped());
        state.descriptors.insert(std::move(entry));
        ++state.descriptor_evictions;
        return;
    }

    try
    {
        const auto recent = state.descriptor_recency.insert(state.descriptor_recency.end(), key);
        try
        {
            state.descriptors.emplace(key, DescriptorEntry{cbv, recent});
        }
        catch (...)
        {
            state.descriptor_recency.erase(recent);
            throw;
        }
    }
    catch (...)
    {
        ++state.descriptor_overflows;
    }
}

void forget_descriptors(Device &state, DescriptorKey start, std::uint32_t count)
{
    auto entry = state.descriptors.lower_bound(start);
    while (entry != state.descriptors.end() && inside_descriptor_range(entry->first, start, count))
        entry = erase_descriptor(state, entry);
}

void on_init_resource(api::device *device, const api::resource_desc &desc, const api::subresource_data *,
                      api::resource_usage, api::resource resource) noexcept
{
    if (resource.handle == 0 || desc.type != api::resource_type::buffer || desc.buffer.size < sizeof(Matrices) ||
        (desc.heap != api::memory_heap::upload && desc.heap != api::memory_heap::gpu_upload))
        return;
    if (correction_requested() && device->get_api() == api::device_api::d3d12)
    {
        auto *native = reinterpret_cast<ID3D12Resource *>(resource.handle);
        wheel_runtime::register_upload(reinterpret_cast<ID3D12Device *>(device->get_native()), native,
                                       desc.buffer.size, native->GetGPUVirtualAddress());
    }
    if (!trace_requested())
        return;
    inspect(device, [&](Device &state) {
        if (state.buffers.size() < buffer_limit || state.buffers.contains(resource.handle))
            state.buffers.insert_or_assign(resource.handle, Buffer{desc.buffer.size, ++state.next_lifetime, desc.heap});
        else
            ++state.buffer_overflows;
    });
}

void on_destroy_resource(api::device *device, api::resource resource) noexcept
{
    if (resource.handle != 0 && correction_requested() && device->get_api() == api::device_api::d3d12)
        wheel_runtime::unregister_upload(reinterpret_cast<ID3D12Device *>(device->get_native()),
                                        reinterpret_cast<ID3D12Resource *>(resource.handle));
    inspect(device, [&](Device &state) {
        state.buffers.erase(resource.handle);
        if (state.view_references.erase(resource.handle) != 0)
            std::erase_if(state.views, [&](const auto &entry) { return entry.second.resource.handle == resource.handle; });
        auto &association = state.depth_association;
        if (association.motion == resource.handle)
            association.motion = 0;
        for (auto &depth : association.observed_fsr_depth)
            if (depth == resource.handle)
                depth = 0;
        auto &copies = association.full_copies;
        const auto end = copies.begin() + association.copies_seen;
        association.copies_seen = static_cast<unsigned>(std::remove_if(copies.begin(), end,
            [&](const FullDepthCopy &copy) { return copy.source == resource.handle || copy.destination == resource.handle; }) - copies.begin());
    });
}

void forget_view(Device &state, api::resource_view view)
{
    const auto entry = state.views.find(view.handle);
    if (entry == state.views.end())
        return;
    const auto references = state.view_references.find(entry->second.resource.handle);
    if (references != state.view_references.end() && --references->second == 0)
        state.view_references.erase(references);
    state.views.erase(entry);
}

void on_init_view(api::device *device, api::resource resource, api::resource_usage usage,
                  const api::resource_view_desc &desc, api::resource_view view) noexcept
{
    if (usage != api::resource_usage::render_target && usage != api::resource_usage::depth_stencil &&
        usage != api::resource_usage::depth_stencil_read)
        return;
    inspect(device, [&](Device &state) {
        forget_view(state, view);
        if (resource.handle == 0)
            return;
        if (state.views.size() == view_limit)
        {
            ++state.view_overflows;
            return;
        }
        auto &references = state.view_references[resource.handle];
        try
        {
            state.views.emplace(view.handle, TargetView{resource, desc.texture.first_level, desc.texture.first_layer,
                usage != api::resource_usage::render_target});
            ++references;
        }
        catch (...)
        {
            if (references == 0)
                state.view_references.erase(resource.handle);
            throw;
        }
    });
}

void on_destroy_view(api::device *device, api::resource_view view) noexcept
{
    inspect(device, [&](Device &state) { forget_view(state, view); });
}

bool on_update_descriptors(api::device *device, std::uint32_t count,
                           const api::descriptor_table_update *updates) noexcept
{
    inspect(device, [&](Device &state) {
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto &update = updates[i];
            DescriptorKey start{};
            if (update.array_offset != 0 || !descriptor_key(device, update.table, update.binding, start))
                continue;
            if (update.type != api::descriptor_type::constant_buffer || update.descriptors == nullptr ||
                update.count > UINT32_MAX - start.second)
            {
                forget_descriptors(state, start, update.count);
                continue;
            }
            const auto *ranges = static_cast<const api::buffer_range *>(update.descriptors);
            for (std::uint32_t j = 0; j < update.count; ++j)
            {
                const DescriptorKey key{start.first, start.second + j};
                const auto &range = ranges[j];
                const auto buffer = state.buffers.find(range.buffer.handle);
                if (range.size >= sizeof(Matrices) && buffer != state.buffers.end())
                    remember_descriptor(state, key, ConstantBuffer{range, buffer->second.lifetime});
                else if (const auto entry = state.descriptors.find(key); entry != state.descriptors.end())
                    erase_descriptor(state, entry);
            }
        }
    });
    return false;
}

bool on_copy_descriptors(api::device *device, std::uint32_t count,
                         const api::descriptor_table_copy *copies) noexcept
{
    inspect(device, [&](Device &state) {
        auto &matched = state.copy_snapshot;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto &copy = copies[i];
            DescriptorKey source{}, dest{};
            if (copy.dest_array_offset != 0 || !descriptor_key(device, copy.dest_table, copy.dest_binding, dest))
                continue;
            if (copy.source_array_offset != 0 || !descriptor_key(device, copy.source_table, copy.source_binding, source) ||
                copy.count > UINT32_MAX - dest.second)
            {
                forget_descriptors(state, dest, copy.count);
                continue;
            }
            // D3D12 requires the source and destination ranges not to overlap.
            // Stage the sparse entries because inserting a destination can evict a source.
            matched.clear();
            try
            {
                for (auto entry = state.descriptors.lower_bound(source);
                     entry != state.descriptors.end() && inside_descriptor_range(entry->first, source, copy.count); ++entry)
                {
                    const auto &cbv = entry->second.cbv;
                    const auto buffer = state.buffers.find(cbv.range.buffer.handle);
                    if (buffer == state.buffers.end() || buffer->second.lifetime != cbv.lifetime)
                        continue;
                    const DescriptorKey target{dest.first, dest.second + (entry->first.second - source.second)};
                    matched.emplace_back(target, cbv);
                    touch_descriptor(state, entry->second);
                }
            }
            catch (...)
            {
                forget_descriptors(state, dest, copy.count);
                ++state.descriptor_overflows;
                continue;
            }

            // Invalidate copied positions whose source was not recorded. Preserve
            // known destination nodes for the common case of rotating table slots.
            auto dest_entry = state.descriptors.lower_bound(dest);
            auto source_entry = matched.begin();
            while (dest_entry != state.descriptors.end() && inside_descriptor_range(dest_entry->first, dest, copy.count))
            {
                while (source_entry != matched.end() && source_entry->first < dest_entry->first)
                    ++source_entry;
                if (source_entry != matched.end() && dest_entry->first == source_entry->first)
                    ++dest_entry;
                else
                    dest_entry = erase_descriptor(state, dest_entry);
            }
            for (const auto &[key, cbv] : matched)
                remember_descriptor(state, key, cbv);
        }
    });
    return false;
}

void on_init_pipeline(api::device *device, api::pipeline_layout layout, std::uint32_t count,
                      const api::pipeline_subobject *subobjects, api::pipeline pipeline) noexcept
{
    if (device->get_api() != api::device_api::d3d12)
        return;
    Pipeline match{0, 0, layout};
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto &subobject = subobjects[i];
        if (subobject.count != 1 || subobject.data == nullptr)
            continue;
        if (subobject.type == api::pipeline_subobject_type::vertex_shader)
            match.vs = recognize(*static_cast<const api::shader_desc *>(subobject.data), wheel_vs);
        if (subobject.type == api::pipeline_subobject_type::pixel_shader)
            match.ps = recognize(*static_cast<const api::shader_desc *>(subobject.data), wheel_ps);
    }
    if (match.vs != 0 || match.ps != 0)
        inspect(device, [&](Device &state) { state.pipelines.insert_or_assign(pipeline.handle, match); });
}

void on_destroy_pipeline(api::device *device, api::pipeline pipeline) noexcept
{
    inspect(device, [&](Device &state) { state.pipelines.erase(pipeline.handle); });
}

void on_init_layout(api::device *device, std::uint32_t count, const api::pipeline_layout_param *params,
                    api::pipeline_layout layout) noexcept
{
    if (count <= wheel_root_parameter || params == nullptr)
        return;
    const auto &param = params[wheel_root_parameter];
    const auto wheel_binding = [](const api::descriptor_range &range) {
        return range.type == api::descriptor_type::constant_buffer && range.binding == wheel_descriptor_offset &&
               range.dx_register_index == 11 && range.dx_register_space == 1 && range.count == 1 &&
               (range.visibility & api::shader_stage::vertex) == api::shader_stage::vertex;
    };
    bool found = false;
    if (param.type == api::pipeline_layout_param_type::descriptor_table)
    {
        const auto &table = param.descriptor_table;
        found = table.ranges != nullptr && std::any_of(table.ranges, table.ranges + table.count, wheel_binding);
    }
    if (param.type == api::pipeline_layout_param_type::descriptor_table_with_flags)
    {
        const auto &table = param.descriptor_table_with_flags;
        found = table.ranges != nullptr && std::any_of(table.ranges, table.ranges + table.count, wheel_binding);
    }
    if (found)
        inspect(device, [&](Device &state) { state.wheel_layouts.insert(layout.handle); });
}

void on_destroy_layout(api::device *device, api::pipeline_layout layout) noexcept
{
    inspect(device, [&](Device &state) { state.wheel_layouts.erase(layout.handle); });
}

void reset_recording(api::command_list *command) noexcept
{
    reset_command(command->get_device(), command->get_native());
}

void destroy_recording(api::command_list *command) noexcept
{
    const auto native = command->get_native();
    inspect(command->get_device(), [&](Device &state) {
        state.commands.erase(native);
        state.reset_tracked_commands.erase(native);
    });
}

template <typename Callback>
void inspect_recording(api::command_list *command, Callback &&callback) noexcept
{
    if (any_correction_active.load(std::memory_order_relaxed) || burst_active.load(std::memory_order_relaxed))
        inspect(command->get_device(), [&](Device &state) {
            if (!state.correction_active && state.remaining == 0)
                return;
            const auto native = command->get_native();
            if (native == 0)
                return;
            auto where = state.commands.find(native);
            if (where == state.commands.end())
            {
                if (state.commands.size() == command_limit)
                    return;
                where = state.commands.try_emplace(native).first;
            }
            callback(state, where->second);
        });
}

void on_bind_pipeline(api::command_list *command, api::pipeline_stage stages, api::pipeline pipeline) noexcept
{
    if ((stages & api::pipeline_stage::all_graphics) == static_cast<api::pipeline_stage>(0))
        return;
    inspect_recording(command, [&](Device &state, Recording &recording) {
        const auto match = state.pipelines.find(pipeline.handle);
        recording.pipeline = match == state.pipelines.end() ? Pipeline{} : match->second;
        recording.native_pipeline = match == state.pipelines.end() ? api::pipeline{} : pipeline;
    });
}

void on_bind_tables(api::command_list *command, api::shader_stage stages, api::pipeline_layout layout,
                    std::uint32_t first, std::uint32_t count, const api::descriptor_table *tables,
                    std::uint32_t, const std::uint32_t *) noexcept
{
    if ((stages & api::shader_stage::all_graphics) == static_cast<api::shader_stage>(0))
        return;
    inspect_recording(command, [&](Device &, Recording &recording) {
        if (recording.graphics_layout.handle != layout.handle)
            recording.graphics_tables = {};
        recording.graphics_layout = layout;
        if (tables == nullptr || first >= recording.graphics_tables.size())
            return;
        const auto tracked = std::min(count, static_cast<std::uint32_t>(recording.graphics_tables.size()) - first);
        std::copy_n(tables, tracked, recording.graphics_tables.begin() + first);
    });
}

void capture_outputs(api::command_list *command, Device &state, Recording &recording,
                     UINT count, const api::resource_view *targets, api::resource_view depth)
{
    const auto tracked = state.reset_tracked_commands.find(command->get_native());
    if (tracked == state.reset_tracked_commands.end())
        return;
    auto &native = tracked->second;
    const bool seen = std::exchange(native.targets_observed, false);
    if (!native.tracked || native.in_pass || !seen || targets == nullptr || depth.handle == 0 ||
        native.pending_count != count || native.pending_depth.handle != depth.handle ||
        !std::equal(targets, targets + count, native.pending_targets.begin(), [](auto a, auto b) { return a.handle == b.handle; }) ||
        recording.mip != 0 || recording.layer != 0 || count <= 4)
        return;

    auto *native_device = reinterpret_cast<ID3D12Device *>(command->get_device()->get_native());
    auto *motion = reinterpret_cast<ID3D12Resource *>(recording.rt4.handle);
    auto *scene_depth = reinterpret_cast<ID3D12Resource *>(recording.depth.handle);
    recording.native_targets_bound = motion != nullptr && scene_depth != nullptr;
    const auto surface = wheel_mask::find_surface(native_device, motion, scene_depth);
    if (!surface || !surface->initialized())
        return;
    if (native.rtv_heap == nullptr)
    {
        const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT};
        if (FAILED(native_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&native.rtv_heap))))
            return;
    }
    if (native.dsv_heap == nullptr)
    {
        const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
        if (FAILED(native_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&native.dsv_heap))))
            return;
    }

    const auto rtv_base = native.rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    auto &outputs = recording.outputs;
    for (UINT i = 0; i < count; ++i)
    {
        // These CPU handles are valid during the app's proxied OM bind call.
        // A private copy avoids rebinding a descriptor the app later rewrites.
        if (targets[i].handle == 0)
            return;
        const D3D12_CPU_DESCRIPTOR_HANDLE destination{rtv_base + static_cast<SIZE_T>(i) * state.rtv_stride};
        native_device->CopyDescriptorsSimple(1, destination, {targets[i].handle}, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        outputs.colors[i] = destination;
    }
    outputs.depth = native.dsv_heap->GetCPUDescriptorHandleForHeapStart();
    native_device->CopyDescriptorsSimple(1, outputs.depth, {depth.handle}, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    outputs.scene_depth = scene_depth;
    outputs.count = count;
    outputs.valid = true;
}

void record_targets(api::command_list *command, api::resource_view rt4, api::resource_view depth,
                    UINT count = 0, const api::resource_view *targets = nullptr) noexcept
{
    inspect_recording(command, [&](Device &state, Recording &recording) {
        // The CPU RTV handle can be overwritten after OMSetRenderTargets.
        // Resolve only views observed through init_resource_view. The MinGW and
        // MSVC ABIs disagree on ReShade's aggregate-return virtual methods.
        const auto target = state.views.find(rt4.handle);
        const auto depth_view = state.views.find(depth.handle);
        const TargetView info = target != state.views.end() ? target->second : TargetView{};
        recording.rt4 = info.resource;
        recording.rt4_view = rt4;
        recording.depth_view = depth;
        recording.depth = depth_view != state.views.end() ? depth_view->second.resource : api::resource{};
        recording.mip = info.mip;
        recording.layer = info.layer;
        recording.outputs = {};
        recording.native_targets_bound = false;
        if (state.correction_active && depth_view != state.views.end() && depth_view->second.depth_stencil &&
            depth_view->second.mip == 0 && depth_view->second.layer == 0)
            capture_outputs(command, state, recording, count, targets, depth);
    });
}

void on_bind_targets(api::command_list *command, std::uint32_t count,
                     const api::resource_view *targets, api::resource_view depth) noexcept
{
    record_targets(command, count > 4 && targets != nullptr ? targets[4] : api::resource_view{}, depth, count, targets);
}

bool on_begin_pass(api::command_list *command, std::uint32_t count,
                   const api::render_pass_render_target_desc *targets,
                   const api::render_pass_depth_stencil_desc *depth, api::render_pass_flags) noexcept
{
    record_targets(command, count > 4 && targets != nullptr ? targets[4].view : api::resource_view{},
                   depth != nullptr ? depth->view : api::resource_view{});
    return false;
}

bool on_end_pass(api::command_list *command) noexcept
{
    on_bind_targets(command, 0, nullptr, {});
    return false;
}

void on_bind_viewports(api::command_list *command, std::uint32_t first, std::uint32_t count,
                       const api::viewport *viewports) noexcept
{
    if (first == 0)
        inspect_recording(command, [&](Device &, Recording &recording) {
            recording.viewport = count != 0 && viewports != nullptr ? viewports[0] : api::viewport{};
            recording.single_viewport = count == 1 && viewports != nullptr;
        });
}

void on_bind_index_buffer(api::command_list *command, api::resource buffer, std::uint64_t offset,
                          std::uint32_t) noexcept
{
    inspect_recording(command, [&](Device &, Recording &recording) {
        recording.index_buffer = buffer;
        recording.index_offset = offset;
    });
}

bool resolve_table(api::device *device, ID3D12Device *native_device, api::descriptor_table table,
                   D3D12_DESCRIPTOR_HEAP_TYPE type, unsigned needed, ID3D12DescriptorHeap *&heap,
                   std::uint64_t &byte_offset, UINT &stride) noexcept
{
    DescriptorKey key{};
    if (!descriptor_key(device, table, 0, key))
        return false;
    heap = reinterpret_cast<ID3D12DescriptorHeap *>(key.first);
    const auto desc = heap->GetDesc();
    if (desc.Type != type || (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0 ||
        key.second >= desc.NumDescriptors || needed > desc.NumDescriptors - key.second)
        return false;

    // These are native COM methods with MinGW's explicit aggregate output ABI.
    stride = native_device->GetDescriptorHandleIncrementSize(type);
    const auto gpu_start = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    byte_offset = static_cast<std::uint64_t>(key.second) * stride;
    return stride != 0 && gpu_start != 0 && table.handle >= gpu_start && table.handle - gpu_start == byte_offset;
}

void prepare_replacement(api::command_list *command, const Device &state, const Recording &recording,
                         const Draw &draw, DrawReplacement &result) noexcept
{
    if (!draw.indexed || draw.indirect || draw.indices == 0 || draw.instances != 1 ||
        !recording.pipeline.rigid_tire() || recording.pipeline.ps == 0 || !recording.single_viewport ||
        recording.native_pipeline.handle == 0 || recording.graphics_layout.handle == 0 ||
        (recording.pipeline.layout.handle != 0 && recording.pipeline.layout.handle != recording.graphics_layout.handle) ||
        !state.wheel_layouts.contains(recording.graphics_layout.handle) || recording.index_buffer.handle == 0 ||
        recording.rt4.handle == 0 || recording.mip != 0 || recording.layer != 0)
        return;

    const auto native_command = command->get_native();
    const auto command_state = state.reset_tracked_commands.find(native_command);
    if (command_state == state.reset_tracked_commands.end())
        return;

    const auto target = state.views.find(recording.rt4_view.handle);
    if (target == state.views.end() || target->second.resource.handle != recording.rt4.handle ||
        target->second.layer != recording.layer || target->second.mip != recording.mip)
        return;

    auto *device = command->get_device();
    auto *native_device = reinterpret_cast<ID3D12Device *>(device->get_native());
    if (native_device == nullptr)
        return;

    std::array<ID3D12DescriptorHeap *, 3> heaps{};
    std::uint64_t offset = 0;
    UINT stride = 0;
    if (!resolve_table(device, native_device, recording.graphics_tables[0], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                       1, heaps[0], offset, stride) ||
        !resolve_table(device, native_device, recording.graphics_tables[1], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                       1, heaps[1], offset, stride) ||
        !resolve_table(device, native_device, recording.graphics_tables[wheel_root_parameter], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                       wheel_descriptor_offset + 1, heaps[wheel_root_parameter], offset, stride) ||
        heaps[1] != heaps[wheel_root_parameter])
        return;

    const auto start = heaps[wheel_root_parameter]->GetCPUDescriptorHandleForHeapStart().ptr;
    const auto cbv_offset = offset + static_cast<std::uint64_t>(wheel_descriptor_offset) * stride;
    if (start == 0 || cbv_offset > std::numeric_limits<SIZE_T>::max() - start)
        return;

    result.device = native_device;
    result.command = reinterpret_cast<ID3D12GraphicsCommandList *>(native_command);
    result.bindings.root_signature = reinterpret_cast<ID3D12RootSignature *>(recording.graphics_layout.handle);
    result.bindings.pipeline = reinterpret_cast<ID3D12PipelineState *>(recording.native_pipeline.handle);
    for (std::size_t i = 0; i < recording.graphics_tables.size(); ++i)
        result.bindings.tables[i].ptr = recording.graphics_tables[i].handle;
    result.cbv.ptr = start + static_cast<SIZE_T>(cbv_offset);
    result.geometry = {reinterpret_cast<ID3D12Resource *>(recording.index_buffer.handle), recording.index_offset};
    result.viewport = {recording.viewport.x, recording.viewport.y, recording.viewport.width, recording.viewport.height,
                       recording.viewport.min_depth, recording.viewport.max_depth};
    result.motion_rt = reinterpret_cast<ID3D12Resource *>(recording.rt4.handle);
    const auto &native = command_state->second;
    if (native.tracked && native.blend_known && !native.in_pass && !native.query_active() && recording.outputs.valid)
    {
        result.outputs = recording.outputs;
        result.outputs.blend_factor = native.blend;
    }
}

void observe_wheel_depth(Device &state, const Recording &recording, const DrawReplacement &draw)
{
    auto &association = state.depth_association;
    if (draw.device == nullptr || !recording.native_targets_bound || recording.rt4.handle != association.motion ||
        draw.viewport.TopLeftX != 0.0f || draw.viewport.TopLeftY != 0.0f ||
        draw.viewport.Width != static_cast<float>(association.width) || draw.viewport.Height != static_cast<float>(association.height) ||
        draw.viewport.MinDepth != 0.0f || draw.viewport.MaxDepth != 1.0f || association.targets_overflowed)
        return;

    const auto view = state.views.find(recording.depth_view.handle);
    if (view == state.views.end() || !view->second.depth_stencil || view->second.resource.handle != recording.depth.handle ||
        view->second.mip != 0 || view->second.layer != 0)
        return;
    auto *depth = reinterpret_cast<ID3D12Resource *>(recording.depth.handle);
    const auto end = association.targets.begin() + association.targets_seen;
    if (depth == nullptr || std::any_of(association.targets.begin(), end,
            [&](const WheelDepthTarget &target) { return target.depth.Get() == depth; }))
        return;

    // The allowlisted native creation descriptor is stronger evidence than
    // the ReShade shader observations used to find a possible draw.
    const auto pipeline = wheel_pipeline_hooks::find(draw.bindings.pipeline);
    if (!pipeline || pipeline->original_signature.Get() != draw.bindings.root_signature)
        return;
    if (end == association.targets.end())
    {
        association.targets_overflowed = true;
        return;
    }

    const auto description = depth->GetDesc();
    if (depth == draw.motion_rt || (description.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0)
        return;
    end->description = description;
    end->motion = draw.motion_rt;
    end->depth = depth;
    ++association.targets_seen;
    char message[256];
    std::snprintf(message, sizeof(message),
        "[NativeWheelArc] Observed native wheel depth: motion=%llx source=%llx format=%u size=%llux%u samples=%u flags=0x%x",
        static_cast<unsigned long long>(association.motion), static_cast<unsigned long long>(recording.depth.handle),
        unsigned(end->description.Format), static_cast<unsigned long long>(end->description.Width), end->description.Height,
        end->description.SampleDesc.Count, unsigned(end->description.Flags));
    write_log(message);
}

void on_execute_bundle(api::command_list *command, api::command_list *) noexcept
{
    inspect_recording(command, [&](Device &state, Recording &recording) {
        if (!state.correction_active)
            return;
        // A bundle can leave the parent's current root or input assembler changed.
        recording.pipeline = {};
        recording.native_pipeline = {};
        recording.graphics_layout = {};
        recording.graphics_tables = {};
        recording.index_buffer = {};
        recording.outputs = {};
        recording.native_targets_bound = false;
        const auto tracked = state.reset_tracked_commands.find(command->get_native());
        if (tracked != state.reset_tracked_commands.end())
            tracked->second.blend_known = false;
    });
}

bool copy_upload_matrices(const ConstantBuffer &cbv, const Buffer &buffer, Matrices &matrices)
{
    const auto offset = cbv.range.offset;
    if ((buffer.heap != api::memory_heap::upload && buffer.heap != api::memory_heap::gpu_upload) ||
        offset > buffer.size || sizeof(matrices) > buffer.size - offset ||
        offset > std::numeric_limits<SIZE_T>::max() - sizeof(matrices))
        return false;

    // 'buffer' is from a current init_resource event and its generation agrees
    // with the descriptor. destroy_resource removes it under the same lock.
    auto *resource = reinterpret_cast<ID3D12Resource *>(cbv.range.buffer.handle);
    const auto desc = resource->GetDesc();
    D3D12_HEAP_PROPERTIES properties{};
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || offset > desc.Width ||
        sizeof(matrices) > desc.Width - offset || FAILED(resource->GetHeapProperties(&properties, nullptr)) ||
        (properties.Type != D3D12_HEAP_TYPE_UPLOAD && properties.Type != gpu_upload_heap))
        return false;

    const auto start = static_cast<SIZE_T>(offset);
    const D3D12_RANGE reads{start, start + sizeof(matrices)};
    void *data = nullptr;
    if (FAILED(resource->Map(0, &reads, &data)))
        return false;
    if (data != nullptr)
        std::memcpy(matrices.data(), static_cast<const std::byte *>(data) + start, sizeof(matrices));
    const D3D12_RANGE no_writes{0, 0};
    resource->Unmap(0, &no_writes);
    return data != nullptr && std::all_of(matrices.begin(), matrices.end(), [](float v) { return std::isfinite(v); });
}

bool record_draw(api::command_list *command, Draw draw) noexcept
{
    DrawReplacement replacement;
    inspect_recording(command, [&](Device &state, const Recording &recording) {
        if (recording.pipeline.vs == 0 && recording.pipeline.ps == 0)
            return;
        if (state.correction_active)
        {
            prepare_replacement(command, state, recording, draw, replacement);
            observe_wheel_depth(state, recording, replacement);
        }
        if (state.remaining == 0)
            return;
        if (state.recorded == state.draws.size())
        {
            ++state.omitted;
            return;
        }
        auto *device = command->get_device();
        draw.pipeline = recording.pipeline;
        draw.command = command->get_native();
        draw.viewport = recording.viewport;
        const auto wheel_table = recording.graphics_tables[wheel_root_parameter];
        draw.table = wheel_table.handle;
        draw.rt4 = recording.rt4;
        draw.rt4_view = recording.rt4_view;
        draw.depth = recording.depth;
        draw.mip = recording.mip;
        draw.layer = recording.layer;
        draw.mask_snapshot = recording.outputs.valid;
        draw.native_targets_bound = recording.native_targets_bound;
        if (const auto native = state.reset_tracked_commands.find(draw.command); native != state.reset_tracked_commands.end())
        {
            draw.native_mask_tracking = native->second.tracked && native->second.blend_known;
            draw.native_render_pass = native->second.in_pass;
            draw.native_query = native->second.query_active();
        }
        if (draw.indexed)
        {
            draw.index_buffer = recording.index_buffer.handle;
            draw.index_offset = recording.index_offset;
        }
        draw.layout_verified = state.wheel_layouts.contains(recording.graphics_layout.handle) &&
            (draw.pipeline.layout.handle == 0 || draw.pipeline.layout.handle == recording.graphics_layout.handle);
        DescriptorKey key{};
        if (trace_requested() && !draw.indirect && draw.pipeline.rigid_tire() && draw.layout_verified &&
            descriptor_key(device, wheel_table, wheel_descriptor_offset, key))
        {
            const auto entry = state.descriptors.find(key);
            if (entry != state.descriptors.end())
            {
                const auto &cbv = entry->second.cbv;
                const auto buffer = state.buffers.find(cbv.range.buffer.handle);
                if (buffer != state.buffers.end() && buffer->second.lifetime == cbv.lifetime)
                {
                    touch_descriptor(state, entry->second);
                    draw.cbv = cbv;
                    if (trace_matrices)
                        draw.cpu_snapshot = copy_upload_matrices(draw.cbv, buffer->second, draw.matrices);
                }
            }
        }
        state.draws[state.recorded++] = draw;
    });

    // Borrowed native state remains valid for this pre-draw callback. The
    // runtime may release COM references that re-enter this observer, so keep
    // it outside the observer lock.
    if (replacement.command == nullptr)
        return false;
    struct RecordingGuard
    {
        const bool previous = std::exchange(recording_wheel, true);
        ~RecordingGuard() { recording_wheel = previous; }
    } guard;
    return wheel_runtime::try_draw(replacement.device, replacement.command,
        replacement.bindings, replacement.cbv, replacement.geometry,
        {draw.indices, draw.instances, draw.first_index, draw.base_vertex, draw.first_instance},
        replacement.viewport, replacement.motion_rt, replacement.outputs);
}

bool on_draw(api::command_list *command, std::uint32_t vertices, std::uint32_t instances,
             std::uint32_t first_vertex, std::uint32_t first_instance) noexcept
{
    Draw draw{};
    draw.indices = vertices;
    draw.instances = instances;
    draw.first_index = first_vertex;
    draw.first_instance = first_instance;
    record_draw(command, draw);
    return false;
}

bool on_draw_indexed(api::command_list *command, std::uint32_t indices, std::uint32_t instances,
                     std::uint32_t first_index, std::int32_t base_vertex, std::uint32_t first_instance) noexcept
{
    Draw draw{};
    draw.indices = indices;
    draw.instances = instances;
    draw.first_index = first_index;
    draw.first_instance = first_instance;
    draw.base_vertex = base_vertex;
    draw.indexed = true;
    return record_draw(command, draw);
}

bool on_indirect(api::command_list *command, api::indirect_command type, api::resource,
                 std::uint64_t, std::uint32_t count, std::uint32_t) noexcept
{
    if (type == api::indirect_command::draw || type == api::indirect_command::draw_indexed ||
        type == api::indirect_command::unknown)
    {
        Draw draw{};
        draw.instances = count;
        draw.indirect = true;
        draw.indexed = type == api::indirect_command::draw_indexed;
        record_draw(command, draw);
        if (any_correction_active.load(std::memory_order_relaxed))
            inspect_recording(command, [&](Device &state, Recording &recording) {
                if (!state.correction_active)
                    return;
                // ReShade reports native ExecuteIndirect as unknown. Root
                // arguments and index input used by its signature can reset.
                recording.graphics_tables = {};
                recording.index_buffer = {};
            });
    }
    return false;
}

void on_destroy_device(api::device *device) noexcept
{
    // Releasing retained native resources can re-enter a destroy callback.
    decltype(devices)::node_type retired;
    {
        const std::lock_guard lock(trace_mutex);
        retired = devices.extract(device);
        update_active();
    }
}

void on_finish_present(api::command_queue *queue, api::swapchain *) noexcept
{
    if (burst_active.load(std::memory_order_relaxed))
        inspect(queue->get_device(), [](Device &state) {
            if (state.remaining != 0 && GetTickCount64() - state.armed_tick > capture_timeout_ms)
            {
                state.remaining = 0;
                if (!state.correction_active)
                    state.commands.clear();
                update_active();
            }
        });
}

void record_depth_copy(api::command_list *command, DepthCopy copy) noexcept
{
    const bool burst = burst_active.load(std::memory_order_relaxed);
    const bool associate = !copy.texture_region && any_correction_active.load(std::memory_order_relaxed);
    if ((!burst && !associate) || copy.source.handle == 0 || copy.destination.handle == 0)
        return;
    inspect(command->get_device(), [&](Device &state) {
        if (associate && state.correction_active)
        {
            const FullDepthCopy full{copy.source.handle, copy.destination.handle};
            auto &association = state.depth_association;
            const auto target_end = association.targets.begin() + association.targets_seen;
            const auto &watched = association.observed_fsr_depth;
            if (!association.copies_overflowed && (std::find(watched.begin(), watched.end(), full.destination) != watched.end() ||
                std::any_of(association.targets.begin(), target_end, [&](const WheelDepthTarget &target) {
                    return reinterpret_cast<std::uintptr_t>(target.depth.Get()) == full.source;
                })))
            {
                const auto end = association.full_copies.begin() + association.copies_seen;
                if (std::find(association.full_copies.begin(), end, full) == end)
                {
                    if (end == association.full_copies.end())
                        association.copies_overflowed = true;
                    else
                        association.full_copies[association.copies_seen++] = full;
                }
            }
        }
        if (state.remaining == 0)
            return;
        const auto matches = [&](std::uint64_t depth) {
            return depth == copy.source.handle || depth == copy.destination.handle;
        };
        if (!matches(state.watched_fsr_depth) && !std::any_of(state.draws.begin(), state.draws.begin() + state.recorded,
                [&](const Draw &draw) { return matches(draw.depth.handle); }))
            return;
        if (state.copies_left == 0)
        {
            ++state.copies_omitted;
            return;
        }
        copy.command = command->get_native();
        state.copies[state.copies_recorded++] = copy;
        --state.copies_left;
    });
}

bool on_copy_resource(api::command_list *command, api::resource source, api::resource destination) noexcept
{
    record_depth_copy(command, {.source = source, .destination = destination});
    return false;
}

bool on_copy_texture(api::command_list *command, api::resource source, std::uint32_t source_subresource,
                     const api::subresource_box *source_box, api::resource destination,
                     std::uint32_t destination_subresource, const api::subresource_box *destination_box, api::filter_mode) noexcept
{
    if (burst_active.load(std::memory_order_relaxed))
        record_depth_copy(command, {.source = source, .destination = destination, .source_subresource = source_subresource,
            .destination_subresource = destination_subresource, .source_box = source_box != nullptr ? *source_box : api::subresource_box{},
            .destination_box = destination_box != nullptr ? *destination_box : api::subresource_box{}, .texture_region = true,
            .has_source_box = source_box != nullptr, .has_destination_box = destination_box != nullptr});
    return false;
}

template <typename T>
unsigned long long log_id(T value) { return static_cast<unsigned long long>(value); }

void log_depth_copy(std::uint64_t dispatch, unsigned index, const DepthCopy &copy, const fsr_probe::Sample &sample)
{
    const auto &source = copy.source_box;
    const auto &dest = copy.destination_box;
    char line[512];
    std::snprintf(line, sizeof(line),
        "[NativeWheelTrace] dispatch=%llu depth_copy=%u kind=%s command=%llx same_list=%u src=%llx dst=%llx src_sub=%u dst_sub=%u has_src_box=%u src_box=%u,%u,%u,%u,%u,%u has_dst_box=%u dst_box=%u,%u,%u,%u,%u,%u",
        log_id(dispatch), index, copy.texture_region ? "texture_region" : "resource", log_id(copy.command),
        unsigned(copy.command == reinterpret_cast<std::uintptr_t>(sample.command_list)), log_id(copy.source.handle),
        log_id(copy.destination.handle), copy.source_subresource, copy.destination_subresource,
        unsigned(copy.has_source_box), source.left, source.top, source.front, source.right, source.bottom, source.back,
        unsigned(copy.has_destination_box), dest.left, dest.top, dest.front, dest.right, dest.bottom, dest.back);
    write_log(line);
}

void log_draw(std::uint64_t dispatch, unsigned index, const Draw &draw, const fsr_probe::Sample &sample)
{
    const auto fsr_command = reinterpret_cast<std::uintptr_t>(sample.command_list);
    const auto fsr_motion = reinterpret_cast<std::uintptr_t>(sample.motion_vectors);
    const auto fsr_depth = sample.depth_identity;
    char line[768];
    std::snprintf(line, sizeof(line),
        "[NativeWheelTrace] dispatch=%llu draw=%u vs=%08x ps=%08x command=%llx same_list=%u rt4=%llx rt4_view=%llx mip=%u layer=%u same_motion=%u depth=%llx viewport=%.3f,%.3f,%.3f,%.3f indirect=%u count=%u instances=%u first=%u base_vertex=%d first_instance=%u ib=%llx ib_offset=%llu layout_verified=%u table=%llx cbv=%llx cbv_offset=%llu cbv_size=%llu cpu_snapshot=%u same_fsr_depth=%u native_mask_tracking=%u native_render_pass=%u native_query=%u mask_snapshot=%u native_targets_bound=%u",
        log_id(dispatch), index, draw.pipeline.vs, draw.pipeline.ps, log_id(draw.command),
        static_cast<unsigned>(draw.command == fsr_command), log_id(draw.rt4.handle), log_id(draw.rt4_view.handle),
        draw.mip, draw.layer,
        static_cast<unsigned>(draw.rt4.handle != 0 && draw.rt4.handle == fsr_motion), log_id(draw.depth.handle),
        draw.viewport.x, draw.viewport.y, draw.viewport.width, draw.viewport.height,
        static_cast<unsigned>(draw.indirect), draw.indices, draw.instances, draw.first_index, draw.base_vertex,
        draw.first_instance, log_id(draw.index_buffer), log_id(draw.index_offset),
        static_cast<unsigned>(draw.layout_verified), log_id(draw.table),
        log_id(draw.cbv.range.buffer.handle),
        log_id(draw.cbv.range.offset), log_id(draw.cbv.range.buffer.handle != 0 ? draw.cbv.range.size : 0),
        static_cast<unsigned>(draw.cpu_snapshot), static_cast<unsigned>(draw.depth.handle != 0 && draw.depth.handle == fsr_depth),
        static_cast<unsigned>(draw.native_mask_tracking), static_cast<unsigned>(draw.native_render_pass),
        static_cast<unsigned>(draw.native_query), static_cast<unsigned>(draw.mask_snapshot), static_cast<unsigned>(draw.native_targets_bound));
    write_log(line);
    if (!draw.cpu_snapshot)
        return;
    for (unsigned matrix = 0; matrix < 3; ++matrix)
    {
        const float *m = draw.matrices.data() + matrix * 16;
        std::snprintf(line, sizeof(line),
            "[NativeWheelTrace] dispatch=%llu draw=%u matrix=%u values=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
            log_id(dispatch), index, matrix, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
            m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        write_log(line);
    }
}
} // namespace

bool correction_requested() noexcept
{
    static const bool active = marker_exists(L"NativeWheelBlur.enable");
    return active;
}

void init_device(api::device *device) noexcept
{
    if (device->get_api() != api::device_api::d3d12)
        return;
    try
    {
        if (!trace_requested() && !correction_requested())
            return;
        std::call_once(register_once, [] {
            wchar_t value[2]{};
            trace_matrices = GetEnvironmentVariableW(L"GTA_NATIVE_WHEEL_TRACE_SKIP_MATRICES", value, 2) != 1 || value[0] != L'1';
            using enum reshade::addon_event;
            reshade::register_event<destroy_device>(on_destroy_device);
            reshade::register_event<init_resource>(on_init_resource);
            reshade::register_event<destroy_resource>(on_destroy_resource);
            reshade::register_event<init_resource_view>(on_init_view);
            reshade::register_event<destroy_resource_view>(on_destroy_view);
            reshade::register_event<init_pipeline>(on_init_pipeline);
            reshade::register_event<destroy_pipeline>(on_destroy_pipeline);
            reshade::register_event<init_pipeline_layout>(on_init_layout);
            reshade::register_event<destroy_pipeline_layout>(on_destroy_layout);
            reshade::register_event<reset_command_list>(reset_recording);
            reshade::register_event<destroy_command_list>(destroy_recording);
            reshade::register_event<bind_pipeline>(on_bind_pipeline);
            reshade::register_event<bind_descriptor_tables>(on_bind_tables);
            reshade::register_event<bind_render_targets_and_depth_stencil>(on_bind_targets);
            reshade::register_event<begin_render_pass>(on_begin_pass);
            reshade::register_event<end_render_pass>(on_end_pass);
            reshade::register_event<bind_viewports>(on_bind_viewports);
            reshade::register_event<bind_index_buffer>(on_bind_index_buffer);
            reshade::register_event<draw_indexed>(on_draw_indexed);
            reshade::register_event<draw_or_dispatch_indirect>(on_indirect);
            reshade::register_event<copy_resource>(on_copy_resource);
            reshade::register_event<copy_texture_region>(on_copy_texture);
            reshade::register_event<finish_present>(on_finish_present);
            if (correction_requested())
            {
                reshade::register_event<execute_secondary_command_list>(on_execute_bundle);
                write_log("[NativeWheelBlur] Rigid wheel draw tracking enabled.");
            }
            if (trace_requested())
            {
                reshade::register_event<update_descriptor_tables>(on_update_descriptors);
                reshade::register_event<copy_descriptor_tables>(on_copy_descriptors);
                reshade::register_event<draw>(on_draw);
                write_log("[NativeWheelTrace] Tracking enabled for the next wheel capture request.");
            }
        });
        const std::lock_guard lock(trace_mutex);
        auto [state, inserted] = devices.try_emplace(device);
        if (inserted && correction_requested())
        {
            auto *native = reinterpret_cast<ID3D12Device *>(device->get_native());
            if (native != nullptr)
                state->second.rtv_stride = native->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
    }
    catch (...) { /* Optional trace setup must not affect game initialization. */ }
}

void set_correction_active(api::device *device, bool active) noexcept
{
    DepthAssociation retired;
    inspect(device, [&](Device &state) {
        if (state.correction_active == active)
            return;
        state.correction_active = active;
        if (!active)
            retired = std::exchange(state.depth_association, DepthAssociation{});
        if (state.remaining == 0)
            state.commands.clear();
        update_active();
    });
}

void track_command(api::device *device, std::uint64_t native_command) noexcept
{
    if (native_command == 0 || !correction_requested())
        return;
    bool added = false;
    inspect(device, [&](Device &state) {
        if (state.reset_tracked_commands.size() < command_limit)
            added = state.reset_tracked_commands.try_emplace(native_command).second;
    });
    if (!added)
        return;

    try
    {
        const bool tracked = track_native_outputs(reinterpret_cast<ID3D12GraphicsCommandList *>(native_command));
        inspect(device, [&](Device &state) {
            if (auto found = state.reset_tracked_commands.find(native_command); found != state.reset_tracked_commands.end())
            {
                found->second.tracked = tracked;
                found->second.blend_known = tracked;
            }
        });
    }
    catch (...) {}
}

void reset_command(api::device *device, std::uint64_t native_command) noexcept
{
    inspect(device, [&](Device &state) {
        state.commands.erase(native_command);
        const auto command = state.reset_tracked_commands.find(native_command);
        if (command != state.reset_tracked_commands.end())
        {
            auto &native = command->second;
            native.blend.fill(1.0f);
            native.blend_known = native.tracked;
            native.in_pass = native.targets_observed = false;
            native.active_query_count = 0;
            native.query_tracking_failed = false;
        }
    });
}

std::shared_ptr<wheel_mask::Surface> prepare_wheel_surface(api::device *device,
    const fsr_probe::Sample &sample, bool allow_allocate) noexcept
{
    Microsoft::WRL::ComPtr<ID3D12Resource> source, motion;
    DepthAssociation retired;
    inspect(device, [&](Device &state) {
        if (!state.correction_active)
            return;
        auto &association = state.depth_association;
        const auto motion_identity = reinterpret_cast<std::uintptr_t>(sample.motion_vectors);
        const bool changed = association.context != sample.context_id || association.motion != motion_identity ||
            association.width != sample.render_width || association.height != sample.render_height || sample.reset;
        if (changed)
        {
            retired = std::exchange(association, DepthAssociation{});
            association.context = sample.context_id;
            association.motion = motion_identity;
            association.width = sample.render_width;
            association.height = sample.render_height;
        }
        auto &watched = association.observed_fsr_depth;
        if (sample.depth_valid && sample.depth_identity != 0 && watched[0] != sample.depth_identity)
        {
            watched[1] = watched[0];
            watched[0] = sample.depth_identity;
        }
        const unsigned copy_count = std::exchange(association.copies_seen, 0u);
        const bool copy_overflowed = std::exchange(association.copies_overflowed, false);
        if (changed || association.targets_overflowed || copy_overflowed || !sample.depth_valid ||
            sample.depth == nullptr || sample.display_resolution_motion_vectors)
            return;

        const WheelDepthTarget *candidate = nullptr;
        for (unsigned i = 0; i < association.targets_seen; ++i)
        {
            const auto &entry = association.targets[i];
            if (entry.motion.Get() != sample.motion_vectors || !wheel_mask::same_depth_layout(entry.description, sample.depth_texture))
                continue;
            if (candidate != nullptr)
                return;
            candidate = &entry;
        }
        if (candidate == nullptr)
            return;

        const auto source_identity = reinterpret_cast<std::uintptr_t>(candidate->depth.Get());
        bool matching_copy = false;
        for (unsigned i = 0; i < copy_count; ++i)
        {
            const auto &copy = association.full_copies[i];
            if (copy.destination != sample.depth_identity)
                continue;
            if (copy.source != source_identity)
                return; // A competing source wrote/was recorded for this FSR input.
            matching_copy = true;
        }
        if (candidate->depth.Get() != sample.depth && !matching_copy)
            return;
        source = candidate->depth;
        motion = candidate->motion;
    });
    if (source == nullptr || motion == nullptr)
        return {};

    auto *native_device = reinterpret_cast<ID3D12Device *>(device->get_native());
    if (allow_allocate)
        return wheel_mask::prepare_surface(native_device, motion.Get(), source.Get(), sample.depth_inverted);
    auto surface = wheel_mask::find_surface(native_device, motion.Get(), source.Get());
    return surface && surface->reverse_z() == sample.depth_inverted ? surface : nullptr;
}

void arm(api::device *device, unsigned samples, std::uintptr_t latest_fsr_depth) noexcept
{
    inspect(device, [&](Device &state) {
        if (!trace_requested() && !state.correction_active)
            return;
        if (!state.correction_active)
            state.commands.clear();
        state.recorded = state.omitted = 0;
        state.copies_recorded = state.copies_omitted = 0;
        state.copies_left = copy_limit;
        state.watched_fsr_depth = latest_fsr_depth;
        state.remaining = std::min(samples, 8u);
        state.armed_tick = GetTickCount64();
        update_active();
    });
}

void observe_fsr(api::device *device, const fsr_probe::Sample &sample) noexcept
{
    if (!burst_active.load(std::memory_order_relaxed))
        return;
    inspect(device, [&](Device &state) {
        if (state.remaining == 0)
            return;
        const bool bounds_ok = state.buffer_overflows == 0 && state.view_overflows == 0 && state.descriptor_overflows == 0;
        char line[512];
        std::snprintf(line, sizeof(line),
            "[NativeWheelTrace] dispatch=%llu fsr_command=%llx fsr_motion=%llx candidates=%u recorded=%u omitted=%u descriptor_bounds_ok=%u buffers=%u buffer_overflows=%llu views=%u view_overflows=%llu descriptors=%u descriptor_overflows=%llu descriptor_evictions=%llu descriptor_trace=%u fsr_depth=%llx depth_valid=%u copies=%u copies_omitted=%u",
            log_id(sample.dispatch_id), log_id(reinterpret_cast<std::uintptr_t>(sample.command_list)),
            log_id(reinterpret_cast<std::uintptr_t>(sample.motion_vectors)), static_cast<unsigned>(state.pipelines.size()),
            state.recorded, state.omitted, static_cast<unsigned>(bounds_ok), static_cast<unsigned>(state.buffers.size()),
            log_id(state.buffer_overflows), static_cast<unsigned>(state.views.size()), log_id(state.view_overflows),
            static_cast<unsigned>(state.descriptors.size()), log_id(state.descriptor_overflows), log_id(state.descriptor_evictions),
            static_cast<unsigned>(trace_requested()), log_id(sample.depth_identity), static_cast<unsigned>(sample.depth_valid),
            state.copies_recorded, state.copies_omitted);
        write_log(line);
        for (unsigned i = 0; i < state.recorded; ++i)
            log_draw(sample.dispatch_id, i, state.draws[i], sample);
        for (unsigned i = 0; i < state.copies_recorded; ++i)
            log_depth_copy(sample.dispatch_id, i, state.copies[i], sample);
        state.recorded = state.omitted = 0;
        state.copies_recorded = state.copies_omitted = 0;
        state.watched_fsr_depth = sample.depth_identity;
        if (--state.remaining == 0)
        {
            if (!state.correction_active)
                state.commands.clear();
            update_active();
        }
    });
}
} // namespace wheel_trace
