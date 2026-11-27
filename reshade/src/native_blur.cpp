#include "native_blur.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include <d3dcompiler.h>
#include <reshade.hpp>
#include <wrl/client.h>

#include "fsr_probe.hpp"
#include "native_blur_source.hpp"
#include "native_motion_math.hpp"

using Microsoft::WRL::ComPtr;

namespace native_blur
{
namespace
{
constexpr std::uint64_t scratch_budget = 128ull * 1024 * 1024;
constexpr std::uint32_t maximum_extent = 8192;
constexpr unsigned descriptors_per_slot = 3;

struct Constants
{
    std::uint32_t output_width, output_height;
    std::uint32_t active_motion_width, active_motion_height;
    float motion_scale_x, motion_scale_y;
    float frame_time_ms, exposure_value;
    std::uint32_t exposure_mode, samples;
    float max_radius;
    std::uint32_t display;
    std::uint32_t motion_texture_width, motion_texture_height;
    std::uint32_t padding[2]{};
};
static_assert(sizeof(Constants) == 16 * sizeof(std::uint32_t));

struct Scratch
{
    std::uint32_t width, height;
    DXGI_FORMAT format;
    D3D12_RESOURCE_DESC desc;
    ComPtr<ID3D12Resource> scene, result;
};

struct Readback
{
    ComPtr<ID3D12Resource> resource;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT before{}, after{};
    std::uint64_t byte_count = 0, row_bytes = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool collected = false;
};

bool supported_color_format(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return true;
    default:
        return false;
    }
}

void transition(ID3D12GraphicsCommandList *command, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    command->ResourceBarrier(1, &barrier);
}

D3D12_TEXTURE_COPY_LOCATION texture_location(ID3D12Resource *resource)
{
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = resource;
    location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    return location;
}

void copy_readback(ID3D12GraphicsCommandList *command, const D3D12_TEXTURE_COPY_LOCATION &source,
                   const Readback &readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint,
                   const D3D12_BOX *box)
{
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    command->CopyTextureRegion(&destination, 0, 0, 0, &source, box);
}

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES props{};
    props.Type = type;
    props.CreationNodeMask = props.VisibleNodeMask = 1;
    return props;
}

void create_srv(ID3D12Device *device, ID3D12Resource *resource, DXGI_FORMAT format,
                D3D12_CPU_DESCRIPTOR_HANDLE destination)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(resource, &desc, destination);
}
} // namespace

struct Engine::State
{
    explicit State(ID3D12Device *device) : device(device) {}

    ID3D12Device *device;
    ComPtr<ID3D12RootSignature> signature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    unsigned descriptor_stride = 0;
    bool initialization_attempted = false;
    bool logged_output = false;
    bool warned_output = false;
    bool warned_surface = false;
    std::uint64_t scratch_bytes = 0;
    // Recorded lists may be replayed until reset. Keep all shapes alive for
    // this device, bounded by the scratch budget.
    std::vector<Scratch> scratch;
    std::array<Readback, Engine::capture_count> readbacks;
    Stats counters;

    bool initialize();
    const Scratch *get_scratch(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format);
    Readback *prepare_readback(unsigned index, const Scratch &surface);
};

bool Engine::State::initialize()
{
    if (initialization_attempted)
        return descriptors != nullptr;
    initialization_attempted = true;

    ComPtr<ID3DBlob> shader, errors;
    if (FAILED(D3DCompile(native_blur_source, sizeof(native_blur_source) - 1, "NativeMotionBlurCS.hlsl",
                          nullptr, nullptr, "main", "cs_5_1",
                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors)))
    {
        reshade::log::message(reshade::log::level::warning, "[NativeBlurPreHUD] Could not compile the native blur shader.");
        if (errors != nullptr)
            reshade::log::message(reshade::log::level::warning, static_cast<const char *>(errors->GetBufferPointer()));
        ++counters.allocation_failures;
        return false;
    }

    constexpr std::array ranges{
        D3D12_DESCRIPTOR_RANGE{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        D3D12_DESCRIPTOR_RANGE{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
    std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, sizeof(Constants) / sizeof(std::uint32_t)};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {static_cast<UINT>(ranges.size()), ranges.data()};

    std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
    for (auto &sampler : samplers)
    {
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
    }
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].ShaderRegister = 1;

    const D3D12_ROOT_SIGNATURE_DESC signature_desc{
        static_cast<UINT>(parameters.size()), parameters.data(), static_cast<UINT>(samplers.size()),
        samplers.data(), D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized;
    if (FAILED(D3D12SerializeRootSignature(&signature_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&signature))))
    {
        reshade::log::message(reshade::log::level::warning, "[NativeBlurPreHUD] Could not create the blur root signature.");
        ++counters.allocation_failures;
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = signature.Get();
    pipeline_desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline))))
    {
        reshade::log::message(reshade::log::level::warning, "[NativeBlurPreHUD] Could not create the blur pipeline.");
        ++counters.allocation_failures;
        return false;
    }

    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        Engine::slot_count * descriptors_per_slot, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&descriptors))))
    {
        reshade::log::message(reshade::log::level::warning, "[NativeBlurPreHUD] Could not allocate blur descriptors.");
        ++counters.allocation_failures;
        return false;
    }
    descriptor_stride = device->GetDescriptorHandleIncrementSize(heap_desc.Type);
    reshade::log::message(reshade::log::level::info, "[NativeBlurPreHUD] Native blur compute pipeline initialized.");
    return true;
}

const Scratch *Engine::State::get_scratch(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format)
{
    for (const auto &entry : scratch)
        if (entry.width == width && entry.height == height && entry.format == format)
            return &entry;

    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = format;
    constexpr unsigned flags = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
        D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
        (support.Support1 & flags) != flags || (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
        return nullptr;

    D3D12_RESOURCE_DESC scene_desc{};
    scene_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    scene_desc.Width = width;
    scene_desc.Height = height;
    scene_desc.DepthOrArraySize = 1;
    scene_desc.MipLevels = 1;
    scene_desc.Format = format;
    scene_desc.SampleDesc.Count = 1;
    auto result_desc = scene_desc;
    result_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const auto scene_size = device->GetResourceAllocationInfo(0, 1, &scene_desc).SizeInBytes;
    const auto result_size = device->GetResourceAllocationInfo(0, 1, &result_desc).SizeInBytes;
    const auto available = scratch_budget - scratch_bytes;
    if (scene_size > available || result_size > available - scene_size)
    {
        ++counters.allocation_failures;
        return nullptr;
    }

    Scratch entry{width, height, format, scene_desc, {}, {}};
    const auto heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &scene_desc,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&entry.scene))) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &result_desc,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&entry.result))))
    {
        ++counters.allocation_failures;
        return nullptr;
    }
    entry.scene->SetName(L"GTA V Native Motion Blur scene input");
    entry.result->SetName(L"GTA V Native Motion Blur scene result");
    scratch.push_back(std::move(entry));
    scratch_bytes += scene_size + result_size;
    return &scratch.back();
}

Readback *Engine::State::prepare_readback(unsigned index, const Scratch &surface)
{
    if (index == 0 || index > readbacks.size() || readbacks[index - 1].resource != nullptr)
        return nullptr;

    Readback candidate;
    std::uint64_t footprint_bytes = 0;
    device->GetCopyableFootprints(&surface.desc, 0, 1, 0, &candidate.before, nullptr,
                                  &candidate.row_bytes, &footprint_bytes);
    if (footprint_bytes == 0 || footprint_bytes > scratch_budget)
        return nullptr;
    candidate.after = candidate.before;
    constexpr auto alignment_mask = std::uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    candidate.after.Offset = (footprint_bytes + alignment_mask) & ~alignment_mask;
    candidate.byte_count = candidate.after.Offset + footprint_bytes;
    candidate.format = surface.format;

    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = candidate.byte_count;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&candidate.resource))))
    {
        ++counters.allocation_failures;
        return nullptr;
    }
    auto &readback = readbacks[index - 1];
    readback = std::move(candidate);
    return &readback;
}

Engine::Engine(ID3D12Device *device) : state_(std::make_unique<State>(device)) {}
Engine::~Engine() = default;

bool Engine::record(ID3D12GraphicsCommandList *command, const fsr_probe::Sample &sample,
                    ID3D12Resource *motion, unsigned slot, const Controls &controls,
                    unsigned capture_index) noexcept
{
    auto &state = *state_;
    const auto skip = [&state] {
        ++state.counters.skipped;
        return false;
    };

    auto *output = sample.output;
    const float exposure = controls.exposure_mode == 1 ? controls.shutter_ms : controls.shutter_angle;
    if (command == nullptr || motion == nullptr || slot >= slot_count ||
        !std::isfinite(exposure) || exposure <= 0.f || !std::isfinite(controls.max_radius) || controls.max_radius <= 0.f ||
        !gtav::native_motion::blur_sample_valid(sample.frame_time_ms, sample.reset, sample.jitter_cancellation))
        return skip();

    const auto &output_desc = sample.output_texture;
    if (!state.logged_output)
    {
        char message[256];
        std::snprintf(message, sizeof(message),
            "[NativeBlurPreHUD] FSR output: valid=%d format=%u texture=%llux%u active=%ux%u state=0x%x flags=0x%x.",
            int(sample.output_valid), unsigned(output_desc.Format), static_cast<unsigned long long>(output_desc.Width),
            unsigned(output_desc.Height), unsigned(sample.upscale_width), unsigned(sample.upscale_height),
            unsigned(sample.output_state), unsigned(output_desc.Flags));
        reshade::log::message(reshade::log::level::info, message);
        state.logged_output = true;
    }

    const auto motion_desc = motion->GetDesc();
    const auto width = sample.upscale_width, height = sample.upscale_height;
    const auto active_motion_width = sample.display_resolution_motion_vectors ? width : sample.render_width;
    const auto active_motion_height = sample.display_resolution_motion_vectors ? height : sample.render_height;
    if (!sample.output_valid || output == nullptr || output == motion ||
        output_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || output_desc.DepthOrArraySize != 1 ||
        output_desc.MipLevels != 1 || output_desc.SampleDesc.Count != 1 ||
        (output_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 ||
        !supported_color_format(output_desc.Format) || width == 0 || height == 0 || width > maximum_extent ||
        height > maximum_extent || width > output_desc.Width || height > output_desc.Height ||
        active_motion_width == 0 || active_motion_height == 0 || active_motion_width > motion_desc.Width ||
        active_motion_height > motion_desc.Height || motion_desc.Width > maximum_extent || motion_desc.Height > maximum_extent ||
        !std::isfinite(sample.scale_x) || !std::isfinite(sample.scale_y))
    {
        if (!std::exchange(state.warned_output, true))
            reshade::log::message(reshade::log::level::warning, "[NativeBlurPreHUD] Skipping an unsupported FSR output state, format, or dimensions.");
        return skip();
    }

    const Scratch *scratch = nullptr;
    Readback *readback = nullptr;
    try
    {
        if (!state.initialize())
            return skip();
        scratch = state.get_scratch(width, height, output_desc.Format);
        if (scratch == nullptr)
        {
            if (!std::exchange(state.warned_surface, true))
                reshade::log::message(reshade::log::level::warning, "[NativeBlurPreHUD] Blur scratch textures unavailable for this format or resolution.");
            return skip();
        }
        if (capture_index != 0)
            readback = state.prepare_readback(capture_index, *scratch);
    }
    catch (...)
    {
        ++state.counters.allocation_failures;
        return skip();
    }

    const auto descriptor_offset = slot * descriptors_per_slot * state.descriptor_stride;
    auto cpu_handle = state.descriptors->GetCPUDescriptorHandleForHeapStart();
    cpu_handle.ptr += descriptor_offset;
    create_srv(state.device, scratch->scene.Get(), scratch->format, cpu_handle);
    cpu_handle.ptr += state.descriptor_stride;
    create_srv(state.device, motion, motion_desc.Format, cpu_handle);
    cpu_handle.ptr += state.descriptor_stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = scratch->format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    state.device->CreateUnorderedAccessView(scratch->result.Get(), nullptr, &uav, cpu_handle);

    const auto scene = texture_location(scratch->scene.Get());
    const auto result = texture_location(scratch->result.Get());
    const auto target = texture_location(output);
    const D3D12_BOX active_output{0, 0, 0, width, height, 1};

    transition(command, output, sample.output_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(command, scene.pResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    command->CopyTextureRegion(&scene, 0, 0, 0, &target, &active_output);
    if (readback != nullptr)
        copy_readback(command, target, *readback, readback->before, &active_output);
    transition(command, scene.pResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const Constants constants{
        width, height, active_motion_width, active_motion_height,
        sample.scale_x / static_cast<float>(active_motion_width), sample.scale_y / static_cast<float>(active_motion_height),
        sample.frame_time_ms, exposure, controls.exposure_mode == 1 ? 1u : 0u,
        static_cast<std::uint32_t>(std::clamp(controls.samples, 3, 17)), std::min(controls.max_radius, 32.f),
        controls.display == 1 ? 1u : 0u, static_cast<std::uint32_t>(motion_desc.Width), motion_desc.Height};
    auto gpu_handle = state.descriptors->GetGPUDescriptorHandleForHeapStart();
    gpu_handle.ptr += descriptor_offset;
    auto *heap = state.descriptors.Get();
    command->SetDescriptorHeaps(1, &heap);
    command->SetComputeRootSignature(state.signature.Get());
    command->SetPipelineState(state.pipeline.Get());
    command->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(std::uint32_t), &constants, 0);
    command->SetComputeRootDescriptorTable(1, gpu_handle);
    command->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    transition(command, result.pResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(command, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    command->CopyTextureRegion(&target, 0, 0, 0, &result, &active_output);
    if (readback != nullptr)
        copy_readback(command, result, *readback, readback->after, &active_output);
    transition(command, result.pResource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(command, output, D3D12_RESOURCE_STATE_COPY_DEST, sample.output_state);
    ++state.counters.drawn;
    return true;
}

bool Engine::capture_recorded(unsigned index) const noexcept
{
    return index != 0 && index <= state_->readbacks.size() && state_->readbacks[index - 1].resource != nullptr;
}

bool Engine::collect(unsigned index, Capture &capture) noexcept
{
    if (!capture_recorded(index))
        return false;
    auto &readback = state_->readbacks[index - 1];
    if (readback.collected)
        return false;
    try
    {
        const auto &footprint = readback.before.Footprint;
        Capture result;
        result.width = footprint.Width;
        result.height = footprint.Height;
        result.format = readback.format;
        const auto row_bytes = static_cast<std::size_t>(readback.row_bytes);
        result.before.resize(row_bytes * result.height);
        result.after.resize(result.before.size());
        void *mapping = nullptr;
        const D3D12_RANGE read_range{0, static_cast<SIZE_T>(readback.byte_count)};
        if (FAILED(readback.resource->Map(0, &read_range, &mapping)))
            return false;
        const auto *pixels = static_cast<const std::uint8_t *>(mapping);
        for (std::size_t y = 0; y < result.height; ++y)
        {
            const auto row_offset = y * footprint.RowPitch;
            std::memcpy(result.before.data() + y * row_bytes, pixels + readback.before.Offset + row_offset, row_bytes);
            std::memcpy(result.after.data() + y * row_bytes, pixels + readback.after.Offset + row_offset, row_bytes);
        }
        constexpr D3D12_RANGE no_write{0, 0};
        readback.resource->Unmap(0, &no_write);
        capture = std::move(result);
        readback.collected = true;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

Stats Engine::stats() const noexcept
{
    return state_->counters;
}
} // namespace native_blur
