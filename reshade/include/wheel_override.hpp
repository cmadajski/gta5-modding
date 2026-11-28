#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <d3d12.h>
#include <wrl/client.h>

namespace wheel_mask { class Surface; }

namespace wheel_override
{
struct GraphicsBindings
{
    ID3D12RootSignature *root_signature = nullptr;
    ID3D12PipelineState *pipeline = nullptr;
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> tables{};
};

struct IndexedDraw
{
    UINT index_count, instance_count, first_index;
    INT base_vertex;
    UINT first_instance;
};

struct GraphicsShaders
{
    ID3D12RootSignature *root_signature = nullptr;
    D3D12_SHADER_BYTECODE vs{}, ps{};
};

struct OutputBindings
{
    ID3D12Resource *scene_depth = nullptr;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> colors{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth{};
    std::array<float, 4> blend_factor{1, 1, 1, 1};
    UINT count = 0;
    bool valid = false;
};

struct MaskDraw
{
    const OutputBindings &original;
    const wheel_mask::Surface &surface;
    std::uint32_t wheel_id;
};

// Borrowed shader storage is valid only for the lifetime of the source stream.
bool inspect_graphics_stream(const D3D12_PIPELINE_STATE_STREAM_DESC &stream, GraphicsShaders &shaders) noexcept;

bool accepts_root_layout(std::span<const std::byte> serialized_root) noexcept;

class Pipeline
{
public:
    // 'serialized_root' is the actual creation blob for original.pRootSignature.
    // 'original' is the exact native descriptor for 'original_pipeline'. Only
    // verified, rigid wheel shader pairs are eligible for the caller to clone.
    HRESULT initialize(ID3D12Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &original,
                       ID3D12PipelineState *original_pipeline, std::span<const std::byte> serialized_root) noexcept;
    HRESULT initialize(ID3D12Device2 *device, const D3D12_PIPELINE_STATE_STREAM_DESC &original,
                       ID3D12PipelineState *original_pipeline, std::span<const std::byte> serialized_root) noexcept;

    // Called on the native direct list with complete current graphics bindings.
    // 'wheel_constants' addresses a CPU-filled, 256-byte-aligned slice owned by
    // the add-on. Keep the slice, this pipeline and its signature alive until GPU
    // completion and command-list replacement. Leave the game's heaps bound.
    bool draw_indexed(ID3D12GraphicsCommandList *command, const GraphicsBindings &current,
                      D3D12_GPU_VIRTUAL_ADDRESS wheel_constants, const IndexedDraw &draw,
                      const MaskDraw *mask = nullptr) const noexcept;

    bool supports_mask(bool reverse_z) const noexcept { return mask_pipelines_[reverse_z] != nullptr; }

private:
    ID3D12RootSignature *original_signature_ = nullptr;
    ID3D12PipelineState *original_pipeline_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> mask_pipelines_;
};
} // namespace wheel_override
