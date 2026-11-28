#include "wheel_override.hpp"
#include "wheel_mask.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace wheel_override
{
namespace
{
constexpr UINT wheel_table = 2;
constexpr UINT wheel_range = 7;
constexpr UINT wheel_root_cbv = 3;

constexpr size_t stream_capacity = 4096;
constexpr size_t missing = stream_capacity;

template <typename T>
struct alignas(void *) StreamEntry
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T value;
};

struct StreamLayout
{
    GraphicsShaders shaders;
    size_t signature = missing, cache = missing;
    size_t blend = missing, formats = missing, depth_format = missing, depth = missing;
    bool depth_v1 = false;
    DXGI_SAMPLE_DESC samples{1, 0};
    D3D12_STREAM_OUTPUT_DESC stream_output{};
    D3D12_VIEW_INSTANCING_DESC view_instances{};
    UINT node_mask = 0;
};

size_t stream_entry_size(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type)
{
    switch (type)
    {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return sizeof(StreamEntry<ID3D12RootSignature *>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return sizeof(StreamEntry<D3D12_SHADER_BYTECODE>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return sizeof(StreamEntry<D3D12_STREAM_OUTPUT_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return sizeof(StreamEntry<D3D12_BLEND_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return sizeof(StreamEntry<UINT>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return sizeof(StreamEntry<D3D12_RASTERIZER_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return sizeof(StreamEntry<D3D12_DEPTH_STENCIL_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return sizeof(StreamEntry<D3D12_DEPTH_STENCIL_DESC1>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return sizeof(StreamEntry<D3D12_INPUT_LAYOUT_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return sizeof(StreamEntry<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return sizeof(StreamEntry<D3D12_PRIMITIVE_TOPOLOGY_TYPE>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return sizeof(StreamEntry<D3D12_RT_FORMAT_ARRAY>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return sizeof(StreamEntry<DXGI_FORMAT>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return sizeof(StreamEntry<DXGI_SAMPLE_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return sizeof(StreamEntry<D3D12_CACHED_PIPELINE_STATE>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return sizeof(StreamEntry<D3D12_PIPELINE_STATE_FLAGS>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return sizeof(StreamEntry<D3D12_VIEW_INSTANCING_DESC>);
    default: return 0;
    }
}

template <typename T>
T stream_value(const std::byte *entry)
{
    T value;
    std::memcpy(&value, entry + offsetof(StreamEntry<T>, value), sizeof(value));
    return value;
}

bool parse_stream(const D3D12_PIPELINE_STATE_STREAM_DESC &stream, StreamLayout &layout)
{
    if (stream.pPipelineStateSubobjectStream == nullptr || stream.SizeInBytes > stream_capacity)
        return false;
    const auto *bytes = static_cast<const std::byte *>(stream.pPipelineStateSubobjectStream);
    std::uint64_t seen = 0;
    for (size_t offset = 0; offset < stream.SizeInBytes;)
    {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        const size_t remaining = stream.SizeInBytes - offset;
        if (remaining < sizeof(type))
            return false;
        const auto *entry = bytes + offset;
        std::memcpy(&type, entry, sizeof(type));
        const size_t size = stream_entry_size(type);
        if (size == 0 || size > remaining || static_cast<unsigned>(type) >= 64 || (seen & (1ull << type)) != 0)
            return false;
        seen |= 1ull << type;

        switch (type)
        {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
            layout.signature = offset + offsetof(StreamEntry<ID3D12RootSignature *>, value);
            layout.shaders.root_signature = stream_value<ID3D12RootSignature *>(entry);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
            layout.shaders.vs = stream_value<D3D12_SHADER_BYTECODE>(entry);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            layout.shaders.ps = stream_value<D3D12_SHADER_BYTECODE>(entry);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            if (stream_value<D3D12_SHADER_BYTECODE>(entry).BytecodeLength != 0)
                return false;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
            layout.node_mask = stream_value<UINT>(entry);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: layout.blend = offset; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: layout.formats = offset; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: layout.depth_format = offset; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
            layout.depth_v1 = true;
            [[fallthrough]];
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
            layout.depth = offset;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
            layout.samples = stream_value<DXGI_SAMPLE_DESC>(entry);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
            layout.stream_output = stream_value<D3D12_STREAM_OUTPUT_DESC>(entry);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
            layout.view_instances = stream_value<D3D12_VIEW_INSTANCING_DESC>(entry);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
            layout.cache = offset + offsetof(StreamEntry<D3D12_CACHED_PIPELINE_STATE>, value);
            break;
        default: break; // All other recognized subobjects are preserved byte for byte.
        }
        offset += size;
    }

    const auto valid_shader = [](const D3D12_SHADER_BYTECODE &shader) {
        return shader.pShaderBytecode != nullptr && shader.BytecodeLength != 0;
    };
    return layout.shaders.root_signature != nullptr && valid_shader(layout.shaders.vs) && valid_shader(layout.shaders.ps);
}

bool matches_range(const D3D12_DESCRIPTOR_RANGE1 &range, D3D12_DESCRIPTOR_RANGE_TYPE type,
                   UINT shader_register, UINT space, UINT offset)
{
    return range.RangeType == type && range.BaseShaderRegister == shader_register &&
        range.RegisterSpace == space && range.NumDescriptors == 1 && range.Flags == D3D12_DESCRIPTOR_RANGE_FLAG_NONE &&
        (range.OffsetInDescriptorsFromTableStart == offset ||
         range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);
}

bool rigid_root_layout(const D3D12_ROOT_SIGNATURE_DESC1 &desc)
{
    if (desc.NumParameters != 3 || desc.NumStaticSamplers != 0 ||
        desc.Flags != D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)
        return false;
    for (UINT i = 0; i < desc.NumParameters; ++i)
    {
        const auto &parameter = desc.pParameters[i];
        if (parameter.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
            parameter.ShaderVisibility != D3D12_SHADER_VISIBILITY_ALL)
            return false;
    }

    const auto &sampler_table = desc.pParameters[0].DescriptorTable;
    if (sampler_table.NumDescriptorRanges != 1)
        return false;
    const auto &samplers = sampler_table.pDescriptorRanges[0];
    if (samplers.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER || samplers.NumDescriptors != UINT32_MAX ||
        samplers.BaseShaderRegister != 0 || samplers.RegisterSpace != 2 ||
        samplers.Flags != D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE || samplers.OffsetInDescriptorsFromTableStart != 0)
        return false;

    const auto &scene = desc.pParameters[1].DescriptorTable;
    constexpr std::array<UINT, 6> scene_cbvs{6, 5, 9, 2, 7, 3};
    const bool alpha_clip = scene.NumDescriptorRanges == scene_cbvs.size() + 1;
    if (!alpha_clip && scene.NumDescriptorRanges != scene_cbvs.size())
        return false;
    if (alpha_clip && !matches_range(scene.pDescriptorRanges[0], D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0))
        return false;
    for (UINT i = 0; i < scene_cbvs.size(); ++i)
    {
        const UINT offset = i + static_cast<UINT>(alpha_clip);
        if (!matches_range(scene.pDescriptorRanges[offset], D3D12_DESCRIPTOR_RANGE_TYPE_CBV, scene_cbvs[i], 0, offset))
            return false;
    }

    const auto &material = desc.pParameters[wheel_table].DescriptorTable;
    if (material.NumDescriptorRanges != wheel_range + 1)
        return false;
    constexpr std::array<UINT, 5> material_srvs{17, 0, 16, 15, 18};
    constexpr std::array<UINT, 3> material_cbvs{12, 10, 11};
    for (UINT i = 0; i < material_srvs.size(); ++i)
        if (!matches_range(material.pDescriptorRanges[i], D3D12_DESCRIPTOR_RANGE_TYPE_SRV, material_srvs[i], 1, i))
            return false;
    for (UINT i = 0; i < material_cbvs.size(); ++i)
    {
        const auto offset = i + static_cast<UINT>(material_srvs.size());
        if (!matches_range(material.pDescriptorRanges[offset], D3D12_DESCRIPTOR_RANGE_TYPE_CBV, material_cbvs[i], 1, offset))
            return false;
    }
    return true;
}

const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *verified_root(ID3D12VersionedRootSignatureDeserializer *deserializer)
{
    const auto *desc = deserializer->GetUnconvertedRootSignatureDesc();
    return desc != nullptr && desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_1 && rigid_root_layout(desc->Desc_1_1) ? desc : nullptr;
}

HRESULT create_signature(ID3D12Device *device, UINT node_mask, std::span<const std::byte> serialized_root,
                         ID3D12RootSignature **signature)
{
    ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
    auto hr = D3D12CreateVersionedRootSignatureDeserializer(serialized_root.data(), serialized_root.size(), IID_PPV_ARGS(&deserializer));
    if (FAILED(hr))
        return hr;
    const auto *original = verified_root(deserializer.Get());
    if (original == nullptr)
        return E_INVALIDARG;

    const auto &desc = original->Desc_1_1;
    std::array<D3D12_ROOT_PARAMETER1, 4> parameters{};
    std::copy_n(desc.pParameters, desc.NumParameters, parameters.begin());
    // b11 is the last singleton range, so the other descriptors keep their offsets.
    --parameters[wheel_table].DescriptorTable.NumDescriptorRanges;
    const auto &wheel = desc.pParameters[wheel_table].DescriptorTable.pDescriptorRanges[wheel_range];
    parameters[wheel_root_cbv].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[wheel_root_cbv].ShaderVisibility = desc.pParameters[wheel_table].ShaderVisibility;
    parameters[wheel_root_cbv].Descriptor = {
        wheel.BaseShaderRegister, wheel.RegisterSpace, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};

    auto relocated = *original;
    relocated.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    relocated.Desc_1_1.pParameters = parameters.data();
    ComPtr<ID3DBlob> blob;
    hr = D3D12SerializeVersionedRootSignature(&relocated, &blob, nullptr);
    if (FAILED(hr))
        return hr;
    return device->CreateRootSignature(node_mask, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(signature));
}

void bind_tables(ID3D12GraphicsCommandList *command, const GraphicsBindings &state)
{
    for (UINT i = 0; i < state.tables.size(); ++i)
        command->SetGraphicsRootDescriptorTable(i, state.tables[i]);
}

template <typename T>
bool simple_depth(const T &depth)
{
    // A meaningful game stencil test cannot be reconstructed using our D32
    // target. Restrict replay to draws whose coverage does not depend on it.
    return depth.DepthEnable && depth.DepthFunc != D3D12_COMPARISON_FUNC_NEVER &&
        (!depth.StencilEnable || (depth.FrontFace.StencilFunc == D3D12_COMPARISON_FUNC_ALWAYS &&
                                 depth.BackFace.StencilFunc == D3D12_COMPARISON_FUNC_ALWAYS));
}

D3D12_BLEND_DESC mask_blend()
{
    const D3D12_RENDER_TARGET_BLEND_DESC no_write{
        FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, 0};
    D3D12_BLEND_DESC blend{};
    blend.IndependentBlendEnable = TRUE;
    std::fill(std::begin(blend.RenderTarget), std::end(blend.RenderTarget), no_write);
    auto &stamp = blend.RenderTarget[2];
    stamp.BlendEnable = TRUE;
    stamp.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
    stamp.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALPHA;
    return blend;
}

constexpr D3D12_RT_FORMAT_ARRAY mask_formats{{wheel_mask::mask_format, wheel_mask::mask_format, wheel_mask::mask_format}, 3};

template <typename T>
T mask_depth(T depth, bool reverse_z)
{
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = reverse_z ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_LESS_EQUAL;
    depth.StencilEnable = FALSE;
    return depth;
}

template <typename T>
void replace_stream_value(std::byte *storage, size_t offset, const T &value)
{
    std::memcpy(storage + offset + offsetof(StreamEntry<T>, value), &value, sizeof(value));
}
} // namespace

bool inspect_graphics_stream(const D3D12_PIPELINE_STATE_STREAM_DESC &stream, GraphicsShaders &shaders) noexcept
{
    StreamLayout layout;
    if (!parse_stream(stream, layout))
        return false;
    shaders = layout.shaders;
    return true;
}

bool accepts_root_layout(std::span<const std::byte> serialized_root) noexcept
{
    if (serialized_root.empty())
        return false;
    ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
    return SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(serialized_root.data(), serialized_root.size(),
        IID_PPV_ARGS(&deserializer))) && verified_root(deserializer.Get()) != nullptr;
}

HRESULT Pipeline::initialize(ID3D12Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &original,
                             ID3D12PipelineState *original_pipeline, std::span<const std::byte> serialized_root) noexcept
{
    if (device == nullptr || original.pRootSignature == nullptr || original_pipeline == nullptr || serialized_root.empty() ||
        original.VS.pShaderBytecode == nullptr || original.VS.BytecodeLength == 0 ||
        original.PS.pShaderBytecode == nullptr || original.PS.BytecodeLength == 0 ||
        original.DS.BytecodeLength != 0 || original.HS.BytecodeLength != 0 || original.GS.BytecodeLength != 0 ||
        signature_ != nullptr)
        return E_INVALIDARG;

    ComPtr<ID3D12RootSignature> signature;
    const auto root_result = create_signature(device, original.NodeMask, serialized_root, &signature);
    if (FAILED(root_result))
        return root_result;
    auto desc = original;
    desc.pRootSignature = signature.Get();
    desc.CachedPSO = {}; // Driver cache belongs to the old root signature.
    const auto result = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline_));
    if (FAILED(result))
        return result;
    if (desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 && desc.StreamOutput.NumEntries == 0 && desc.NumRenderTargets >= 3 &&
        desc.DSVFormat != DXGI_FORMAT_UNKNOWN && simple_depth(desc.DepthStencilState))
    {
        auto mask = desc;
        mask.BlendState = mask_blend();
        mask.NumRenderTargets = mask_formats.NumRenderTargets;
        std::copy(std::begin(mask_formats.RTFormats), std::end(mask_formats.RTFormats), std::begin(mask.RTVFormats));
        mask.DSVFormat = wheel_mask::depth_format;
        for (const bool reverse_z : {false, true})
        {
            mask.DepthStencilState = mask_depth(desc.DepthStencilState, reverse_z);
            device->CreateGraphicsPipelineState(&mask, IID_PPV_ARGS(&mask_pipelines_[reverse_z]));
        }
    }
    original_signature_ = original.pRootSignature;
    original_pipeline_ = original_pipeline;
    signature_ = std::move(signature);
    return S_OK;
}

HRESULT Pipeline::initialize(ID3D12Device2 *device, const D3D12_PIPELINE_STATE_STREAM_DESC &original,
                             ID3D12PipelineState *original_pipeline, std::span<const std::byte> serialized_root) noexcept
{
    StreamLayout layout;
    if (device == nullptr || original_pipeline == nullptr || serialized_root.empty() || signature_ != nullptr ||
        !parse_stream(original, layout))
        return E_INVALIDARG;

    ComPtr<ID3D12RootSignature> signature;
    const auto root_result = create_signature(device, layout.node_mask, serialized_root, &signature);
    if (FAILED(root_result))
        return root_result;

    alignas(void *) std::array<std::byte, stream_capacity> storage;
    std::memcpy(storage.data(), original.pPipelineStateSubobjectStream, original.SizeInBytes);
    auto *replacement = signature.Get();
    std::memcpy(storage.data() + layout.signature, &replacement, sizeof(replacement));
    if (layout.cache != missing)
    {
        constexpr D3D12_CACHED_PIPELINE_STATE no_cache{};
        std::memcpy(storage.data() + layout.cache, &no_cache, sizeof(no_cache));
    }

    const D3D12_PIPELINE_STATE_STREAM_DESC stream{original.SizeInBytes, storage.data()};
    const auto result = device->CreatePipelineState(&stream, IID_PPV_ARGS(&pipeline_));
    if (FAILED(result))
        return result;
    if (layout.samples.Count == 1 && layout.samples.Quality == 0 && layout.view_instances.ViewInstanceCount == 0 &&
        layout.stream_output.NumEntries == 0 &&
        layout.blend != missing && layout.formats != missing && layout.depth != missing && layout.depth_format != missing &&
        stream_value<D3D12_RT_FORMAT_ARRAY>(storage.data() + layout.formats).NumRenderTargets >= 3 &&
        stream_value<DXGI_FORMAT>(storage.data() + layout.depth_format) != DXGI_FORMAT_UNKNOWN)
    {
        replace_stream_value(storage.data(), layout.blend, mask_blend());
        replace_stream_value(storage.data(), layout.formats, mask_formats);
        replace_stream_value(storage.data(), layout.depth_format, wheel_mask::depth_format);
        const auto clone_depth = [&]<typename T>() {
            const auto depth = stream_value<T>(storage.data() + layout.depth);
            if (!simple_depth(depth))
                return;
            for (const bool reverse_z : {false, true})
            {
                replace_stream_value(storage.data(), layout.depth, mask_depth(depth, reverse_z));
                device->CreatePipelineState(&stream, IID_PPV_ARGS(&mask_pipelines_[reverse_z]));
            }
        };
        if (layout.depth_v1)
            clone_depth.template operator()<D3D12_DEPTH_STENCIL_DESC1>();
        else
            clone_depth.template operator()<D3D12_DEPTH_STENCIL_DESC>();
    }
    original_signature_ = layout.shaders.root_signature;
    original_pipeline_ = original_pipeline;
    signature_ = std::move(signature);
    return S_OK;
}

bool Pipeline::draw_indexed(ID3D12GraphicsCommandList *command, const GraphicsBindings &current,
                            D3D12_GPU_VIRTUAL_ADDRESS wheel_constants, const IndexedDraw &draw,
                            const MaskDraw *mask) const noexcept
{
    if (command == nullptr || command->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || pipeline_ == nullptr ||
        current.root_signature != original_signature_ || current.pipeline != original_pipeline_ ||
        wheel_constants == 0 || wheel_constants % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT != 0 ||
        std::any_of(current.tables.begin(), current.tables.end(), [](const auto &table) { return table.ptr == 0; }))
        return false;

    command->SetGraphicsRootSignature(signature_.Get());
    command->SetPipelineState(pipeline_.Get());
    bind_tables(command, current);
    command->SetGraphicsRootConstantBufferView(wheel_root_cbv, wheel_constants);
    command->DrawIndexedInstanced(draw.index_count, draw.instance_count, draw.first_index, draw.base_vertex, draw.first_instance);

    if (mask != nullptr && mask->original.valid && mask->surface.initialized() &&
        mask->original.scene_depth != nullptr && mask->original.scene_depth == mask->surface.scene_depth() &&
        mask->original.depth.ptr != 0 && mask->original.count != 0 && mask->original.count <= mask->original.colors.size() &&
        mask->wheel_id != 0 && mask->wheel_id <= wheel_mask::id_scale && supports_mask(mask->surface.reverse_z()))
    {
        const auto &targets = mask->surface.targets();
        const auto depth = mask->surface.depth_target();
        command->SetPipelineState(mask_pipelines_[mask->surface.reverse_z()].Get());
        const float factor[]{1, 1, 1, static_cast<float>(mask->wheel_id) / wheel_mask::id_scale};
        command->OMSetBlendFactor(factor);
        command->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), FALSE, &depth);
        command->DrawIndexedInstanced(draw.index_count, draw.instance_count, draw.first_index, draw.base_vertex, draw.first_instance);
        command->OMSetRenderTargets(mask->original.count, mask->original.colors.data(), FALSE, &mask->original.depth);
        command->OMSetBlendFactor(mask->original.blend_factor.data());
    }

    command->SetGraphicsRootSignature(current.root_signature);
    command->SetPipelineState(current.pipeline);
    bind_tables(command, current);
    return true;
}
} // namespace wheel_override
