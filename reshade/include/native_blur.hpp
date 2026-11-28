#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <d3d12.h>

namespace fsr_probe { struct Sample; }
namespace wheel_mask { class Surface; }

namespace native_blur
{
struct Controls
{
    int exposure_mode = 0;
    float shutter_angle = 180.f;
    float shutter_ms = 8.33f;
    int samples = 9;
    float max_radius = 12.f;
    int display = 0;
    bool enabled = true;
    bool wheel_arcs = true;
};

struct WheelArcInputs
{
    wheel_mask::Surface *surface = nullptr;
    ID3D12Resource *parameters = nullptr;
    std::uint32_t parameter_count = 0, frame_id = 0;
};

struct Capture
{
    std::uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::vector<std::uint8_t> before, after;
    std::uint32_t motion_width = 0, motion_height = 0;
    DXGI_FORMAT motion_format = DXGI_FORMAT_UNKNOWN;
    std::vector<std::uint8_t> motion;
    std::vector<std::uint8_t> wheel_mask, wheel_depth, scene_depth, wheel_parameters;
    std::uint32_t wheel_width = 0, wheel_height = 0, wheel_parameter_stride = 0;
    DXGI_FORMAT scene_depth_format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t scene_depth_row_bytes = 0;
};

struct Stats
{
    std::uint64_t drawn = 0, skipped = 0, allocation_failures = 0;
    std::uint64_t arc_frames = 0, arc_clears = 0;
};

class Engine
{
public:
    static constexpr unsigned slot_count = 32;
    static constexpr unsigned capture_count = 8;

    explicit Engine(ID3D12Device *device);
    ~Engine();

    // Caller serializes access and ensures recordings execute on one graphics queue.
    // Motion is an owned texture in combined pixel and non-pixel shader resource state.
    // Reuse a descriptor slot only after its recording was replaced and GPU use completed.
    // With controls disabled, only a requested capture is recorded and the scene is untouched.
    bool record(ID3D12GraphicsCommandList *command, const fsr_probe::Sample &sample,
                ID3D12Resource *motion, unsigned slot, const Controls &controls,
                unsigned capture_index = 0, const WheelArcInputs *arcs = nullptr) noexcept;

    bool capture_recorded(unsigned index) const noexcept;
    // Caller ensures GPU use completed and the command-list recording was replaced.
    // Successful collection releases the capture slot for reuse.
    bool collect(unsigned index, Capture &capture) noexcept;
    Stats stats() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace native_blur
