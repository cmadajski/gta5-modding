#include "wheel_pipeline_hooks.hpp"

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace wheel_pipeline_hooks
{
namespace
{
constexpr size_t max_devices = 4;
constexpr size_t max_roots = 64;
constexpr size_t max_root_bytes = 16 * 1024;
constexpr size_t max_pipelines = 64;

struct ShaderId
{
    size_t size;
    std::uint32_t crc;
};

constexpr ShaderId rigid{6136, 0xDF51595B}, alpha_clip{6160, 0x2C667625};
constexpr std::array rigid_pixels{ShaderId{7188, 0xDEF27DF8}, ShaderId{7220, 0x1B5B11EC}};
constexpr std::array alpha_pixels{ShaderId{8164, 0x5DE20624}, ShaderId{8192, 0x2A35207B}};

constexpr auto crc_table = [] {
    std::array<std::uint32_t, 256> table{};
    for (unsigned i = 0; i < table.size(); ++i)
    {
        std::uint32_t crc = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xEDB88320u : 0u);
        table[i] = crc;
    }
    return table;
}();

bool matches(const D3D12_SHADER_BYTECODE &shader, ShaderId id)
{
    if (shader.pShaderBytecode == nullptr || shader.BytecodeLength != id.size)
        return false;
    const auto *bytes = static_cast<const unsigned char *>(shader.pShaderBytecode);
    std::uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < id.size; ++i)
        crc = (crc >> 8) ^ crc_table[(crc ^ bytes[i]) & 0xffu];
    return ~crc == id.crc;
}

bool eligible(const D3D12_SHADER_BYTECODE &vs, const D3D12_SHADER_BYTECODE &ps)
{
    // These shader pairs share the exact inspected b11 layout on build 25971420.
    const auto match_pixels = [&](const auto &ids) {
        return std::any_of(ids.begin(), ids.end(), [&](ShaderId id) { return matches(ps, id); });
    };
    return (matches(vs, rigid) && match_pixels(rigid_pixels)) || (matches(vs, alpha_clip) && match_pixels(alpha_pixels));
}

struct Root
{
    ComPtr<ID3D12RootSignature> signature;
    std::vector<std::byte> serialized;
};

struct Device
{
    ID3D12Device *base = nullptr, *stream_alias = nullptr;
    bool legacy = false, stream = false;
    std::shared_mutex mutex;
    std::unordered_map<ID3D12RootSignature *, std::shared_ptr<const Root>> roots;
    std::unordered_map<ID3D12PipelineState *, std::shared_ptr<const Match>> pipelines;
};

using CreateRoot = HRESULT (STDMETHODCALLTYPE *)(ID3D12Device *, UINT, const void *, SIZE_T, REFIID, void **);
using CreateGraphics = HRESULT (STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **);
using CreateStream = HRESULT (STDMETHODCALLTYPE *)(ID3D12Device2 *, const D3D12_PIPELINE_STATE_STREAM_DESC *, REFIID, void **);

template <typename Function>
struct NativeHook
{
    std::atomic<Function> original{};
    void *target = nullptr;
    HMODULE module = nullptr;
};

struct Process
{
    std::mutex install;
    std::mutex clone;
    std::shared_mutex devices_mutex;
    std::array<std::shared_ptr<Device>, max_devices> devices;
    NativeHook<CreateRoot> root;
    NativeHook<CreateGraphics> legacy;
    NativeHook<CreateStream> stream;
};

Process &process()
{
    // Detours are installed for the pinned add-on's lifetime. Devices tear down
    // explicitly so COM objects never release during static DLL destruction.
    static auto *state = new Process;
    return *state;
}

thread_local bool cloning = false;

struct CloneGuard
{
    CloneGuard() { cloning = true; }
    ~CloneGuard() { cloning = false; }
};

bool owns_device(const Device &device, ID3D12Device *native)
{
    return device.base == native || device.stream_alias == native;
}

std::shared_ptr<Device> registered(ID3D12Device *native)
{
    auto &state = process();
    const std::shared_lock lock(state.devices_mutex);
    for (const auto &device : state.devices)
        if (device && owns_device(*device, native))
            return device;
    return {};
}

template <typename T>
ComPtr<T> native_result(void *object)
{
    ComPtr<T> result;
    reinterpret_cast<IUnknown *>(object)->QueryInterface(IID_PPV_ARGS(&result));
    return result;
}

void observe_root(ID3D12Device *native, const void *blob, SIZE_T size, void *result)
{
    if (blob == nullptr || size == 0 || size > max_root_bytes)
        return;
    const auto device = registered(native);
    if (!device)
        return;
    {
        const std::shared_lock lock(device->mutex);
        if (device->roots.size() == max_roots)
            return;
    }

    const std::span<const std::byte> bytes{static_cast<const std::byte *>(blob), size};
    if (!wheel_override::accepts_root_layout(bytes))
        return;
    auto signature = native_result<ID3D12RootSignature>(result);
    if (signature == nullptr)
        return;
    auto *identity = signature.Get();
    auto snapshot = std::make_shared<Root>();
    snapshot->signature = std::move(signature);
    snapshot->serialized.assign(bytes.begin(), bytes.end());

    const std::unique_lock lock(device->mutex);
    if (device->roots.size() < max_roots)
        device->roots.try_emplace(identity, std::move(snapshot));
}

template <typename NativeDevice, typename Description>
void observe_pipeline(const std::shared_ptr<Device> &device, NativeDevice *native, const Description &desc,
                      ID3D12RootSignature *root, void *result)
{
    if (root == nullptr)
        return;
    auto pipeline = native_result<ID3D12PipelineState>(result);
    if (pipeline == nullptr)
        return;
    auto *identity = pipeline.Get();

    // Serialize only the rare matching clones. Do not hold the registry mutex
    // across a driver call, since creation can synchronously re-enter a hook.
    const std::lock_guard clone_lock(process().clone);
    std::shared_ptr<const Root> snapshot;
    {
        const std::shared_lock lock(device->mutex);
        if (device->pipelines.size() == max_pipelines || device->pipelines.contains(identity))
            return;
        const auto found = device->roots.find(root);
        if (found == device->roots.end())
            return;
        snapshot = found->second;
    }

    const CloneGuard guard;
    auto match = std::make_shared<Match>();
    match->original_pipeline = std::move(pipeline);
    match->original_signature = snapshot->signature;
    if (FAILED(match->replacement.initialize(native, desc, identity, snapshot->serialized)))
        return;
    const std::unique_lock lock(device->mutex);
    device->pipelines.try_emplace(identity, std::move(match));
}

HRESULT STDMETHODCALLTYPE create_root(ID3D12Device *native, UINT node_mask, const void *blob, SIZE_T size,
                                     REFIID riid, void **output) noexcept
{
    const auto result = process().root.original.load(std::memory_order_acquire)(native, node_mask, blob, size, riid, output);
    if (SUCCEEDED(result) && output != nullptr && *output != nullptr && !cloning)
        try { observe_root(native, blob, size, *output); }
        catch (...) { /* Preserve native creation results when observing fails. */ }
    return result;
}

HRESULT STDMETHODCALLTYPE create_graphics(ID3D12Device *native, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                         REFIID riid, void **output) noexcept
{
    const auto result = process().legacy.original.load(std::memory_order_acquire)(native, desc, riid, output);
    if (SUCCEEDED(result) && desc != nullptr && output != nullptr && *output != nullptr && !cloning)
        try
        {
            if (eligible(desc->VS, desc->PS))
                if (const auto device = registered(native); device && device->legacy)
                    observe_pipeline(device, native, *desc, desc->pRootSignature, *output);
        }
        catch (...) { /* Preserve native creation results when observing fails. */ }
    return result;
}

HRESULT STDMETHODCALLTYPE create_stream(ID3D12Device2 *native, const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                                       REFIID riid, void **output) noexcept
{
    const auto result = process().stream.original.load(std::memory_order_acquire)(native, desc, riid, output);
    if (SUCCEEDED(result) && desc != nullptr && output != nullptr && *output != nullptr && !cloning)
        try
        {
            if (const auto device = registered(native); device && device->stream)
            {
                wheel_override::GraphicsShaders shaders;
                if (wheel_override::inspect_graphics_stream(*desc, shaders) && eligible(shaders.vs, shaders.ps))
                    observe_pipeline(device, native, *desc, shaders.root_signature, *output);
            }
        }
        catch (...) { /* Preserve native creation results when observing fails. */ }
    return result;
}

void *method(IUnknown *object, size_t index)
{
    return (*reinterpret_cast<void *const *const *>(object))[index];
}

template <typename Function, typename Detour>
bool install(NativeHook<Function> &hook, void *target, Detour detour)
{
    if (hook.target != nullptr)
        return hook.target == target;
    HMODULE module = nullptr;
    if (target == nullptr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(target), &module))
        return false;
    void *trampoline = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void *>(detour), &trampoline) != MH_OK)
    {
        FreeLibrary(module);
        return false;
    }
    hook.original.store(reinterpret_cast<Function>(trampoline), std::memory_order_release);
    if (MH_EnableHook(target) != MH_OK)
    {
        MH_RemoveHook(target);
        hook.original.store(nullptr, std::memory_order_relaxed);
        FreeLibrary(module);
        return false;
    }
    hook.target = target;
    hook.module = module; // Keep the trampoline's native module mapped while the detour is installed.
    return true;
}
} // namespace

bool init(ID3D12Device *native_device) noexcept
{
    if (native_device == nullptr)
        return false;
    try
    {
        auto &state = process();
        const std::lock_guard install_lock(state.install);
        if (registered(native_device))
            return true;
        auto device = std::make_shared<Device>();
        device->base = native_device;
        // Public COM ABI offsets from the ID3D12Device and ID3D12Device2 vtables.
        if (!install(state.root, method(native_device, 16), create_root))
            return false;
        device->legacy = install(state.legacy, method(native_device, 10), create_graphics);

        ComPtr<ID3D12Device2> stream_device;
        if (SUCCEEDED(native_device->QueryInterface(IID_PPV_ARGS(&stream_device))))
        {
            device->stream_alias = stream_device.Get();
            device->stream = install(state.stream, method(stream_device.Get(), 47), create_stream);
        }
        if (!device->legacy && !device->stream)
            return false;

        const std::unique_lock lock(state.devices_mutex);
        for (auto &entry : state.devices)
            if (!entry)
            {
                entry = std::move(device);
                return true;
            }
    }
    catch (...) { /* Optional instrumentation cannot prevent device initialization. */ }
    return false;
}

std::shared_ptr<const Match> find(ID3D12PipelineState *native_pipeline) noexcept
{
    if (native_pipeline == nullptr)
        return {};
    try
    {
        auto &state = process();
        const std::shared_lock devices_lock(state.devices_mutex);
        for (const auto &device : state.devices)
            if (device)
            {
                const std::shared_lock lock(device->mutex);
                if (const auto found = device->pipelines.find(native_pipeline); found != device->pipelines.end())
                    return found->second;
            }
    }
    catch (...) { /* A missing clone simply leaves the original draw untouched. */ }
    return {};
}

void shutdown_device(ID3D12Device *native_device) noexcept
{
    if (native_device == nullptr)
        return;
    try
    {
        std::shared_ptr<Device> retired;
        auto &state = process();
        const std::unique_lock lock(state.devices_mutex);
        for (auto &entry : state.devices)
            if (entry && owns_device(*entry, native_device))
            {
                retired = std::move(entry);
                break;
            }
        // Destroy after unlocking. A final COM release may notify other add-ons.
    }
    catch (...) { /* Keep any uncertain lifetime alive. */ }
}
} // namespace wheel_pipeline_hooks
