#define CINTERFACE
#include <windows.h>
#include <initguid.h>

#include "wheel_descriptors.hpp"

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

struct ID3D12Device15;

namespace wheel_descriptors {
namespace {

static_assert(offsetof(ID3D12DeviceVtbl, CreateConstantBufferView) == 17 * sizeof(void *));
static_assert(offsetof(ID3D12DeviceVtbl, CreateShaderResourceView) == 18 * sizeof(void *));
static_assert(offsetof(ID3D12DeviceVtbl, CreateUnorderedAccessView) == 19 * sizeof(void *));
static_assert(offsetof(ID3D12DeviceVtbl, CopyDescriptors) == 23 * sizeof(void *));
static_assert(offsetof(ID3D12DeviceVtbl, CopyDescriptorsSimple) == 24 * sizeof(void *));

using CreateCbv = decltype(ID3D12DeviceVtbl::CreateConstantBufferView);
using CreateSrv = decltype(ID3D12DeviceVtbl::CreateShaderResourceView);
using CreateUav = decltype(ID3D12DeviceVtbl::CreateUnorderedAccessView);
using CopyDescriptors = decltype(ID3D12DeviceVtbl::CopyDescriptors);
using CopySimple = decltype(ID3D12DeviceVtbl::CopyDescriptorsSimple);
using TryCreateCbv = HRESULT (STDMETHODCALLTYPE *)(ID3D12Device15 *, const D3D12_CONSTANT_BUFFER_VIEW_DESC *,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE);
using TryCreateSrv = HRESULT (STDMETHODCALLTYPE *)(ID3D12Device15 *, ID3D12Resource *,
                                                   const D3D12_SHADER_RESOURCE_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
using TryCreateUav = HRESULT (STDMETHODCALLTYPE *)(ID3D12Device15 *, ID3D12Resource *, ID3D12Resource *,
                                                   const D3D12_UNORDERED_ACCESS_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
using TryCreateFeedback = HRESULT (STDMETHODCALLTYPE *)(ID3D12Device15 *, ID3D12Resource *, ID3D12Resource *,
                                                        D3D12_CPU_DESCRIPTOR_HANDLE);

// The Windows ID3D12Device15 COM layout in Microsoft's DirectX-Headers.
constexpr std::size_t try_srv_slot = 85, try_uav_slot = 86, try_cbv_slot = 87, try_feedback_slot = 91;
#ifdef __ID3D12Device15_INTERFACE_DEFINED__
static_assert(offsetof(ID3D12Device15Vtbl, TryCreateShaderResourceView) == try_srv_slot * sizeof(void *));
static_assert(offsetof(ID3D12Device15Vtbl, TryCreateUnorderedAccessView) == try_uav_slot * sizeof(void *));
static_assert(offsetof(ID3D12Device15Vtbl, TryCreateConstantBufferView) == try_cbv_slot * sizeof(void *));
static_assert(offsetof(ID3D12Device15Vtbl, TryCreateSamplerFeedbackUnorderedAccessView) == try_feedback_slot * sizeof(void *));
static_assert(std::is_same_v<TryCreateSrv, decltype(ID3D12Device15Vtbl::TryCreateShaderResourceView)>);
static_assert(std::is_same_v<TryCreateUav, decltype(ID3D12Device15Vtbl::TryCreateUnorderedAccessView)>);
static_assert(std::is_same_v<TryCreateCbv, decltype(ID3D12Device15Vtbl::TryCreateConstantBufferView)>);
static_assert(std::is_same_v<TryCreateFeedback, decltype(ID3D12Device15Vtbl::TryCreateSamplerFeedbackUnorderedAccessView)>);
#endif
#ifdef __ID3D12Device8_INTERFACE_DEFINED__
static_assert(offsetof(ID3D12Device8Vtbl, CreateSamplerFeedbackUnorderedAccessView) == 71 * sizeof(void *));
using CreateFeedback = decltype(ID3D12Device8Vtbl::CreateSamplerFeedbackUnorderedAccessView);
#endif

constexpr std::size_t max_devices = 4;
constexpr std::size_t max_entries = 131072;
constexpr std::size_t max_copy_descriptors = 1U << 20;
constexpr auto cbv_heap = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
// Query recent interfaces even when the build SDK predates them.
constexpr IID device11_iid{0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};
constexpr IID device12_iid{0x5af5c532, 0x4c91, 0x4cd0, {0xb5, 0x41, 0x15, 0xa4, 0x05, 0x39, 0x5f, 0xc5}};
constexpr IID device13_iid{0x14eecffc, 0x4df8, 0x40f7, {0xa1, 0x18, 0x5c, 0x81, 0x6f, 0x45, 0x69, 0x5e}};
constexpr IID device14_iid{0x5f6e592d, 0xd895, 0x44c2, {0x8e, 0x4a, 0x88, 0xad, 0x49, 0x26, 0xd3, 0x23}};
constexpr IID device15_iid{0x76cff76f, 0x1e9b, 0x4450, {0x8c, 0xdc, 0x34, 0xf1, 0xaf, 0x78, 0x8e, 0x5b}};

struct Payload {
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    UINT size = 0;
};

struct Device {
    struct Entry {
        ConstantBufferView view;
        std::list<SIZE_T>::iterator usage;
    };

    std::array<const void *, 20> aliases{};
    std::size_t alias_count = 0;
    UINT stride = 0;
    std::atomic<bool> enabled{true};
    std::mutex mutex;
    std::uint64_t generation = 0;
    std::unordered_map<SIZE_T, Entry> entries;
    std::list<SIZE_T> recent;

    bool has_alias(const void *pointer) const noexcept
    {
        const auto end = aliases.cbegin() + alias_count;
        return std::find(aliases.cbegin(), end, pointer) != end;
    }

    bool remember_alias(const void *pointer) noexcept
    {
        if (has_alias(pointer))
            return true;
        if (alias_count == aliases.size())
            return false;
        aliases[alias_count++] = pointer;
        return true;
    }

    void clear() noexcept
    {
        entries.clear();
        recent.clear();
    }

    std::optional<ConstantBufferView> read(SIZE_T handle)
    {
        const auto found = entries.find(handle);
        if (found == entries.end())
            return {};
        recent.splice(recent.begin(), recent, found->second.usage);
        return found->second.view;
    }

    void invalidate(SIZE_T handle)
    {
        if (const auto found = entries.find(handle); found != entries.end()) {
            recent.erase(found->second.usage);
            entries.erase(found);
        }
    }

    void write(SIZE_T handle, Payload payload)
    {
        if (payload.address == 0 || handle == 0) {
            invalidate(handle);
            return;
        }
        if (generation == std::numeric_limits<std::uint64_t>::max()) {
            enabled.store(false, std::memory_order_relaxed);
            clear();
            return;
        }
        const ConstantBufferView fresh{payload.address, payload.size, ++generation};
        if (const auto found = entries.find(handle); found != entries.end()) {
            found->second.view = fresh;
            recent.splice(recent.begin(), recent, found->second.usage);
            return;
        }
        if (entries.size() == max_entries)
            invalidate(recent.back());
        recent.push_front(handle);
        entries.emplace(handle, Entry{fresh, recent.begin()});
    }
};

template <typename Function>
struct NativeHook {
    std::atomic<Function> original{};
    Function target = nullptr;
    HMODULE module = nullptr;
};

struct Process {
    std::mutex installation;
    std::shared_mutex devices_mutex;
    std::array<std::shared_ptr<Device>, max_devices> devices;
    std::atomic<bool> healthy{true};
    NativeHook<CreateCbv> create_cbv;
    NativeHook<CreateSrv> create_srv;
    NativeHook<CreateUav> create_uav;
    NativeHook<CopyDescriptors> copy;
    NativeHook<CopySimple> copy_simple;
    NativeHook<TryCreateCbv> try_cbv;
    NativeHook<TryCreateSrv> try_srv;
    NativeHook<TryCreateUav> try_uav;
    NativeHook<TryCreateFeedback> try_feedback;
#ifdef __ID3D12Device8_INTERFACE_DEFINED__
    NativeHook<CreateFeedback> create_feedback;
#endif
};

Process &process()
{
    // Native detours intentionally outlive device registrations.
    static auto *state = new Process;
    return *state;
}

std::shared_ptr<Device> registered(const void *native)
{
    auto &state = process();
    const std::shared_lock lock(state.devices_mutex);
    for (const auto &device : state.devices)
        if (device && device->has_alias(native))
            return device;
    return {};
}

template <typename Operation>
void observe(const void *native, Operation operation) noexcept
{
    auto &state = process();
    if (!state.healthy.load(std::memory_order_relaxed))
        return;
    std::shared_ptr<Device> device;
    try {
        device = registered(native);
        if (!device)
            return;
        const std::lock_guard lock(device->mutex);
        if (device->enabled.load(std::memory_order_relaxed)) {
            try {
                operation(*device);
            } catch (...) {
                // A partial update could otherwise leave older CBV contents in
                // a destination the driver has already overwritten.
                device->clear();
            }
        }
    } catch (...) {
        if (device)
            device->enabled.store(false, std::memory_order_relaxed);
        else
            state.healthy.store(false, std::memory_order_relaxed);
    }
}

Payload cbv_payload(const D3D12_CONSTANT_BUFFER_VIEW_DESC *description) noexcept
{
    constexpr auto alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    constexpr auto max_size = 16 * D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
    if (description == nullptr)
        return {};
    const auto &[address, size] = *description;
    if (address == 0 || address % alignment != 0 || size == 0 || size > max_size ||
        size % alignment != 0 || address > std::numeric_limits<D3D12_GPU_VIRTUAL_ADDRESS>::max() - size)
        return {};
    return {address, size};
}

struct Ranges {
    UINT count;
    const D3D12_CPU_DESCRIPTOR_HANDLE *starts;
    const UINT *sizes;

    UINT size(UINT range) const noexcept { return sizes != nullptr ? sizes[range] : 1; }

    std::optional<std::size_t> total(UINT stride) const noexcept
    {
        if (count > max_copy_descriptors || (count != 0 && starts == nullptr))
            return {};
        std::size_t total = 0;
        for (UINT range = 0; range < count; ++range) {
            const auto length = size(range);
            if (length == 0)
                continue;
            const auto first = starts[range].ptr;
            if (first == 0 || length > max_copy_descriptors - total ||
                length - 1 > (std::numeric_limits<SIZE_T>::max() - first) / stride)
                return {};
            total += length;
        }
        return total;
    }
};

struct Cursor {
    const Ranges &ranges;
    SIZE_T stride;
    UINT range = 0, offset = 0;

    SIZE_T next() noexcept
    {
        while (offset == ranges.size(range)) {
            ++range;
            offset = 0;
        }
        return ranges.starts[range].ptr + stride * offset++;
    }
};

void copy_views(Device &device, Ranges destinations, Ranges sources)
{
    const auto dest_count = destinations.total(device.stride);
    const auto source_count = sources.total(device.stride);
    if (!dest_count || dest_count != source_count) {
        device.clear();
        return;
    }

    // Complete the source snapshot before any destination write can evict a
    // later source from the bounded cache. Missing sources stay empty.
    std::array<Payload, 32> local;
    std::vector<Payload> large;
    if (*dest_count > local.size())
        large.resize(*dest_count);
    auto *const staged = large.empty() ? local.data() : large.data();
    Cursor source{sources, device.stride};
    for (std::size_t i = 0; i < *dest_count; ++i)
        if (const auto known = device.read(source.next()))
            staged[i] = {known->gpu_address, known->size};

    Cursor destination{destinations, device.stride};
    for (std::size_t i = 0; i < *dest_count; ++i)
        device.write(destination.next(), staged[i]);
}

void STDMETHODCALLTYPE create_cbv(ID3D12Device *native, const D3D12_CONSTANT_BUFFER_VIEW_DESC *description,
                                  D3D12_CPU_DESCRIPTOR_HANDLE destination) noexcept
{
    process().create_cbv.original.load(std::memory_order_acquire)(native, description, destination);
    observe(native, [&](Device &device) { device.write(destination.ptr, cbv_payload(description)); });
}

void STDMETHODCALLTYPE create_srv(ID3D12Device *native, ID3D12Resource *resource,
                                  const D3D12_SHADER_RESOURCE_VIEW_DESC *description,
                                  D3D12_CPU_DESCRIPTOR_HANDLE destination) noexcept
{
    process().create_srv.original.load(std::memory_order_acquire)(native, resource, description, destination);
    observe(native, [&](Device &device) { device.invalidate(destination.ptr); });
}

void STDMETHODCALLTYPE create_uav(ID3D12Device *native, ID3D12Resource *resource, ID3D12Resource *counter,
                                  const D3D12_UNORDERED_ACCESS_VIEW_DESC *description,
                                  D3D12_CPU_DESCRIPTOR_HANDLE destination) noexcept
{
    process().create_uav.original.load(std::memory_order_acquire)(native, resource, counter, description, destination);
    observe(native, [&](Device &device) { device.invalidate(destination.ptr); });
}

#ifdef __ID3D12Device8_INTERFACE_DEFINED__
void STDMETHODCALLTYPE create_feedback(ID3D12Device8 *native, ID3D12Resource *target, ID3D12Resource *feedback,
                                       D3D12_CPU_DESCRIPTOR_HANDLE destination) noexcept
{
    process().create_feedback.original.load(std::memory_order_acquire)(native, target, feedback, destination);
    observe(native, [&](Device &device) { device.invalidate(destination.ptr); });
}
#endif

HRESULT STDMETHODCALLTYPE try_create_cbv(ID3D12Device15 *native, const D3D12_CONSTANT_BUFFER_VIEW_DESC *description,
                                         D3D12_CPU_DESCRIPTOR_HANDLE destination) noexcept
{
    const auto status = process().try_cbv.original.load(std::memory_order_acquire)(native, description, destination);
    // An unsuccessful write provides no authoritative metadata for this slot.
    observe(native, [&](Device &device) { device.write(destination.ptr, status == S_OK ? cbv_payload(description) : Payload{}); });
    return status;
}

HRESULT STDMETHODCALLTYPE try_create_srv(ID3D12Device15 *native, ID3D12Resource *resource,
                                         const D3D12_SHADER_RESOURCE_VIEW_DESC *description,
                                         D3D12_CPU_DESCRIPTOR_HANDLE destination) noexcept
{
    const auto status = process().try_srv.original.load(std::memory_order_acquire)(native, resource, description, destination);
    observe(native, [&](Device &device) { device.invalidate(destination.ptr); });
    return status;
}

HRESULT STDMETHODCALLTYPE try_create_uav(ID3D12Device15 *native, ID3D12Resource *resource, ID3D12Resource *counter,
                                         const D3D12_UNORDERED_ACCESS_VIEW_DESC *description,
                                         D3D12_CPU_DESCRIPTOR_HANDLE destination) noexcept
{
    const auto status = process().try_uav.original.load(std::memory_order_acquire)(native, resource, counter, description, destination);
    observe(native, [&](Device &device) { device.invalidate(destination.ptr); });
    return status;
}

HRESULT STDMETHODCALLTYPE try_create_feedback(ID3D12Device15 *native, ID3D12Resource *target, ID3D12Resource *feedback,
                                              D3D12_CPU_DESCRIPTOR_HANDLE destination) noexcept
{
    const auto status = process().try_feedback.original.load(std::memory_order_acquire)(native, target, feedback, destination);
    observe(native, [&](Device &device) { device.invalidate(destination.ptr); });
    return status;
}

thread_local bool copying = false;

struct CopyCall {
    const bool nested = std::exchange(copying, true);
    ~CopyCall() { copying = nested; }
};

void STDMETHODCALLTYPE copy_descriptors(ID3D12Device *native,
                                        UINT dest_count, const D3D12_CPU_DESCRIPTOR_HANDLE *destinations,
                                        const UINT *dest_sizes,
                                        UINT src_count, const D3D12_CPU_DESCRIPTOR_HANDLE *sources,
                                        const UINT *src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) noexcept
{
    const CopyCall call;
    process().copy.original.load(std::memory_order_acquire)(native, dest_count, destinations, dest_sizes,
                                                            src_count, sources, src_sizes, type);
    if (!call.nested && type == cbv_heap)
        observe(native, [&](Device &device) {
            copy_views(device, {dest_count, destinations, dest_sizes}, {src_count, sources, src_sizes});
        });
}

void STDMETHODCALLTYPE copy_simple(ID3D12Device *native, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination,
                                   D3D12_CPU_DESCRIPTOR_HANDLE source, D3D12_DESCRIPTOR_HEAP_TYPE type) noexcept
{
    const CopyCall call;
    process().copy_simple.original.load(std::memory_order_acquire)(native, count, destination, source, type);
    if (!call.nested && type == cbv_heap)
        observe(native, [&](Device &device) { copy_views(device, {1, &destination, &count}, {1, &source, &count}); });
}

template <typename Function, typename Detour>
bool install(NativeHook<Function> &hook, Function target, Detour detour) noexcept
{
    static_assert(std::is_convertible_v<Detour, Function>);
    if (hook.target != nullptr)
        return hook.target == target;
    HMODULE module = nullptr;
    if (target == nullptr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                              reinterpret_cast<LPCWSTR>(target), &module))
        return false;
    auto *const address = reinterpret_cast<void *>(target);
    void *trampoline = nullptr;
    if (MH_CreateHook(address, reinterpret_cast<void *>(detour), &trampoline) != MH_OK) {
        FreeLibrary(module);
        return false;
    }
    hook.original.store(reinterpret_cast<Function>(trampoline), std::memory_order_release);
    if (MH_EnableHook(address) != MH_OK) {
        MH_RemoveHook(address);
        hook.original.store(nullptr, std::memory_order_relaxed);
        FreeLibrary(module);
        return false;
    }
    hook.target = target;
    hook.module = module;
    return true;
}

// The inherited slots must take the same native path through every known
// interface alias. If one differs, a partial observer would return stale data.
bool same_writers(const ID3D12DeviceVtbl &base, const ID3D12DeviceVtbl &alias) noexcept
{
    return base.CreateConstantBufferView == alias.CreateConstantBufferView &&
           base.CreateShaderResourceView == alias.CreateShaderResourceView &&
           base.CreateUnorderedAccessView == alias.CreateUnorderedAccessView &&
           base.CopyDescriptors == alias.CopyDescriptors &&
           base.CopyDescriptorsSimple == alias.CopyDescriptorsSimple;
}

struct AliasInterface {
    const IID *id;
    bool has_feedback = false;
    bool has_try_create = false;
};

template <typename Method>
Method device15_method(ID3D12Device15 *native, std::size_t slot) noexcept
{
    return reinterpret_cast<Method>((*reinterpret_cast<void *const *const *>(native))[slot]);
}

bool install_device15_writers(Process &state, ID3D12Device15 *device) noexcept
{
    return install(state.try_srv, device15_method<TryCreateSrv>(device, try_srv_slot), try_create_srv) &&
           install(state.try_uav, device15_method<TryCreateUav>(device, try_uav_slot), try_create_uav) &&
           install(state.try_cbv, device15_method<TryCreateCbv>(device, try_cbv_slot), try_create_cbv) &&
           install(state.try_feedback, device15_method<TryCreateFeedback>(device, try_feedback_slot), try_create_feedback);
}

bool collect_aliases(ID3D12Device *native, Device &device, Process &state) noexcept
{
    const AliasInterface interfaces[]{
        {&device15_iid, true, true},
        {&IID_ID3D12Device}, {&IID_ID3D12Device1}, {&IID_ID3D12Device2},
        {&IID_ID3D12Device3}, {&IID_ID3D12Device4}, {&IID_ID3D12Device5},
        {&IID_ID3D12Device6}, {&IID_ID3D12Device7},
#ifdef __ID3D12Device8_INTERFACE_DEFINED__
        {&IID_ID3D12Device8, true},
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
        {&IID_ID3D12Device9, true},
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
        {&IID_ID3D12Device10, true},
#endif
        {&device11_iid, true}, {&device12_iid, true}, {&device13_iid, true}, {&device14_iid, true},
    };
    if (!device.remember_alias(native))
        return false;
    for (const auto &version : interfaces) {
        void *pointer = nullptr;
        const auto status = native->lpVtbl->QueryInterface(native, *version.id, &pointer);
        if (status == E_NOINTERFACE && pointer == nullptr)
            continue;
        if (FAILED(status) || pointer == nullptr)
            return false;
        auto *const alias = static_cast<ID3D12Device *>(pointer);
        bool compatible = same_writers(*native->lpVtbl, *alias->lpVtbl) && device.remember_alias(alias);
#ifdef __ID3D12Device8_INTERFACE_DEFINED__
        if (compatible && version.has_feedback) {
            auto *const feedback = static_cast<ID3D12Device8 *>(pointer);
            compatible = install(state.create_feedback, feedback->lpVtbl->CreateSamplerFeedbackUnorderedAccessView,
                                 create_feedback);
        }
#endif
        if (compatible && version.has_try_create)
            compatible = install_device15_writers(state, static_cast<ID3D12Device15 *>(pointer));
        alias->lpVtbl->Release(alias);
        if (!compatible)
            return false;
    }
    return true;
}

} // namespace

bool init(ID3D12Device *native_device) noexcept
{
    if (native_device == nullptr)
        return false;
    try {
        auto &state = process();
        const std::lock_guard lock(state.installation);
        if (!state.healthy.load(std::memory_order_relaxed))
            return false;
        if (const auto existing = registered(native_device))
            return existing->enabled.load(std::memory_order_relaxed);

        auto device = std::make_shared<Device>();
        const auto &methods = *native_device->lpVtbl;
        device->stride = methods.GetDescriptorHandleIncrementSize(native_device, cbv_heap);
        if (device->stride == 0 || !collect_aliases(native_device, *device, state) ||
            !install(state.create_cbv, methods.CreateConstantBufferView, create_cbv) ||
            !install(state.create_srv, methods.CreateShaderResourceView, create_srv) ||
            !install(state.create_uav, methods.CreateUnorderedAccessView, create_uav) ||
            !install(state.copy, methods.CopyDescriptors, copy_descriptors) ||
            !install(state.copy_simple, methods.CopyDescriptorsSimple, copy_simple))
            return false;

        const std::unique_lock devices_lock(state.devices_mutex);
        for (auto &entry : state.devices)
            if (!entry) {
                entry = std::move(device);
                return true;
            }
    } catch (...) { /* Optional observation must leave native device use intact. */ }
    return false;
}

std::optional<ConstantBufferView> lookup(ID3D12Device *native_device,
                                         D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle) noexcept
{
    if (native_device == nullptr || cpu_handle.ptr == 0)
        return {};
    try {
        if (!process().healthy.load(std::memory_order_relaxed))
            return {};
        if (const auto device = registered(native_device)) {
            const std::lock_guard lock(device->mutex);
            if (device->enabled.load(std::memory_order_relaxed))
                return device->read(cpu_handle.ptr);
        }
    } catch (...) { /* Missing metadata leaves the stock draw in place. */ }
    return {};
}

void shutdown_device(ID3D12Device *native_device) noexcept
{
    if (native_device == nullptr)
        return;
    try {
        std::shared_ptr<Device> retired;
        auto &state = process();
        const std::lock_guard installation(state.installation);
        const std::unique_lock lock(state.devices_mutex);
        for (auto &entry : state.devices)
            if (entry && entry->has_alias(native_device)) {
                entry->enabled.store(false, std::memory_order_relaxed);
                retired = std::move(entry);
                break;
            }
    } catch (...) { /* Native device teardown proceeds independently. */ }
}

} // namespace wheel_descriptors
