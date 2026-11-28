#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

namespace gtav::native_motion {

struct float2 {
    float x;
    float y;
};

struct extent2d {
    std::uint32_t width;
    std::uint32_t height;
};

// Do not turn a cut, pause, invalid timing, or uncorrected jitter into blur.
// The diagnostic view may still show these samples for inspection.
inline bool blur_sample_valid(float frame_time_ms, bool reset, bool jitter_cancellation)
{
    return std::isfinite(frame_time_ms) && frame_time_ms > 0.f && frame_time_ms <= 250.f &&
           !reset && !jitter_cancellation;
}

// FSR divides the dispatch scale by the active motion-vector extent to get
// a raw-to-UV scale. Convert that to render pixels for the visualization.
inline std::optional<float2> fsr_render_pixel_scale(
    float2 dispatch_scale, extent2d render_size, extent2d active_motion_size)
{
    if (render_size.width == 0 || render_size.height == 0 ||
        active_motion_size.width == 0 || active_motion_size.height == 0) {
        return std::nullopt;
    }

    const float2 scale{
        dispatch_scale.x * (static_cast<float>(render_size.width) / static_cast<float>(active_motion_size.width)),
        dispatch_scale.y * (static_cast<float>(render_size.height) / static_cast<float>(active_motion_size.height)),
    };
    if (!std::isfinite(scale.x) || !std::isfinite(scale.y)) {
        return std::nullopt;
    }

    return scale;
}

// FSR's active viewport starts at the top-left of its input texture.
inline std::optional<float2> motion_texture_uv_scale(extent2d active_motion_size, extent2d texture_size)
{
    if (active_motion_size.width == 0 || active_motion_size.height == 0 ||
        active_motion_size.width > texture_size.width || active_motion_size.height > texture_size.height) {
        return std::nullopt;
    }

    return float2{
        static_cast<float>(active_motion_size.width) / static_cast<float>(texture_size.width),
        static_cast<float>(active_motion_size.height) / static_cast<float>(texture_size.height),
    };
}

} // namespace gtav::native_motion
