#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <d3d12.h>

namespace fsr_probe { struct Sample; }

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
};

struct Capture
{
    std::uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::vector<std::uint8_t> before, after;
};

struct Stats
{
    std::uint64_t drawn = 0, skipped = 0, allocation_failures = 0;
};

class Engine
{
public:
    static constexpr unsigned slot_count = 32;
    static constexpr unsigned capture_count = 4;

    explicit Engine(ID3D12Device *device);
    ~Engine();

    // Caller serializes access and ensures recordings execute on one graphics queue.
    // Reuse a descriptor slot only after its recording was replaced and GPU use completed.
    bool record(ID3D12GraphicsCommandList *command, const fsr_probe::Sample &sample,
                ID3D12Resource *motion, unsigned slot, const Controls &controls,
                unsigned capture_index = 0) noexcept;

    bool capture_recorded(unsigned index) const noexcept;
    // Caller checks the fence for this captured recording before collecting pixels.
    bool collect(unsigned index, Capture &capture) noexcept;
    Stats stats() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace native_blur
