#pragma once

#include <memory>

#include "wheel_override.hpp"

namespace wheel_pipeline_hooks
{
struct Match
{
    Microsoft::WRL::ComPtr<ID3D12PipelineState> original_pipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> original_signature;
    wheel_override::Pipeline replacement;
};

// Opt in on the native device before it creates game roots and PSOs. The caller
// keeps the hook code loaded and MinHook initialized for the process lifetime.
// Pipeline-library loads and unknown native pipeline streams remain untracked.
bool init(ID3D12Device *native_device) noexcept;

std::shared_ptr<const Match> find(ID3D12PipelineState *native_pipeline) noexcept;

// Call only after the device has finished using every retained command recording.
// The process-wide detours remain installed and pass through untracked devices.
void shutdown_device(ID3D12Device *native_device) noexcept;
} // namespace wheel_pipeline_hooks
