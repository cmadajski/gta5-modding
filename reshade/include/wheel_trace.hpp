#pragma once

#include <cstdint>
#include <memory>

namespace reshade::api { struct device; }
namespace fsr_probe { struct Sample; }
namespace wheel_mask { class Surface; }

namespace wheel_trace
{
// Query at device initialization, outside the DLL loader lock.
bool correction_requested() noexcept;

// Call from the add-on's D3D12 device initialization callback before the game
// creates PSOs and descriptors. The trace is opt-in at process startup via
// NativeWheelTrace.enable beside the game executable or GTA_NATIVE_WHEEL_TRACE=1.
// NativeWheelBlur.enable activates tracked wheel draw substitution separately.
void init_device(reshade::api::device *device) noexcept;

// Enable hot draw tracking only while this device has a working runtime and
// a verified presentation queue. Creation observers remain active at startup.
void set_correction_active(reshade::api::device *device, bool active) noexcept;

// Enroll a tagged native command list only after its Reset implementation has
// been verified to use the installed native Reset hook.
void track_command(reshade::api::device *device, std::uint64_t native_command) noexcept;

// Also call after the original native command-list Reset succeeds, in case
// that path bypasses the ReShade command-list wrapper.
void reset_command(reshade::api::device *device, std::uint64_t native_command) noexcept;

// Resolve the native wheel DSV for this FSR motion input. An observed full
// resource copy can associate it with the FSR depth input. These observations
// are made while recording and do not establish native queue ordering.
std::shared_ptr<wheel_mask::Surface> prepare_wheel_surface(reshade::api::device *device,
    const fsr_probe::Sample &sample, bool allow_allocate) noexcept;

// An accepted wheel burst logs bounded draw and replay provenance while
// correction is active. The trace marker also enables descriptor and matrix
// data. Observe one sample for each accepted capture in the burst.
void arm(reshade::api::device *device, unsigned samples, std::uintptr_t latest_fsr_depth = 0) noexcept;
void observe_fsr(reshade::api::device *device, const fsr_probe::Sample &sample) noexcept;
}
