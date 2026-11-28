#include "native_motion_math.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>

namespace {

using gtav::native_motion::extent2d;
using gtav::native_motion::float2;
using gtav::native_motion::fsr_render_pixel_scale;
using gtav::native_motion::motion_texture_uv_scale;

void expect(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void expect_vec(std::optional<float2> actual, float2 expected, const char *message)
{
    constexpr float tolerance = 0.00001f;
    expect(actual && std::abs(actual->x - expected.x) <= tolerance &&
               std::abs(actual->y - expected.y) <= tolerance,
           message);
}

void expect_motion(float2 raw, float2 scale, extent2d render_size,
                   float2 expected_pixels, float2 expected_uv)
{
    // These are the shader's raw-to-pixels and pixels-to-UV operations.
    const float2 pixels{raw.x * scale.x, raw.y * scale.y};
    expect_vec(pixels, expected_pixels, "wrong render-pixel displacement");
    const float2 uv{
        pixels.x / static_cast<float>(render_size.width),
        pixels.y / static_cast<float>(render_size.height),
    };
    expect_vec(uv, expected_uv, "wrong viewport UV displacement");
}

} // namespace

int main()
{
    constexpr extent2d render{1280, 720};
    constexpr extent2d display{2560, 1440};

    const auto low_resolution_scale = fsr_render_pixel_scale({8.0f, -4.0f}, render, render);
    expect_vec(low_resolution_scale, {8.0f, -4.0f}, "low-res scale must preserve both signed axes");
    expect_motion({0.5f, 0.25f}, *low_resolution_scale, render, {4.0f, -1.0f},
                  {4.0f / 1280.0f, -1.0f / 720.0f});
    expect_motion({-0.5f, -0.25f}, *low_resolution_scale, render, {-4.0f, 1.0f},
                  {-4.0f / 1280.0f, 1.0f / 720.0f});
    expect_motion({0.0f, 0.0f}, *low_resolution_scale, render, {0.0f, 0.0f}, {0.0f, 0.0f});

    const auto high_resolution_scale = fsr_render_pixel_scale({8.0f, 4.0f}, render, display);
    expect_vec(high_resolution_scale, {4.0f, 2.0f}, "display-res scale must account for resolution");
    expect_motion({0.5f, 1.0f}, *high_resolution_scale, render, {2.0f, 2.0f},
                  {2.0f / 1280.0f, 2.0f / 720.0f});
    expect_vec(fsr_render_pixel_scale({8.0f, 4.0f}, {1280, 800}, display), {4.0f, 20.0f / 9.0f},
               "each axis must use its own scale factor");

    expect_vec(motion_texture_uv_scale(render, render), {1.0f, 1.0f}, "cropped textures cover all UVs");
    expect_vec(motion_texture_uv_scale(render, {1920, 1080}), {2.0f / 3.0f, 2.0f / 3.0f},
               "active viewport must exclude texture padding");
    expect_vec(motion_texture_uv_scale({1, 720}, {41, 720}), {1.0f / 41.0f, 1.0f},
               "a one-texel active region must be accepted");
    expect(!motion_texture_uv_scale(render, {1279, 720}), "undersized textures must be rejected");
    expect(!motion_texture_uv_scale({0, 720}, render), "empty active regions must be rejected");
    expect(!motion_texture_uv_scale(render, {1280, 0}), "empty textures must be rejected");

    constexpr float infinity = std::numeric_limits<float>::infinity();
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float max_float = std::numeric_limits<float>::max();
    using gtav::native_motion::blur_sample_valid;
    expect(blur_sample_valid(10.f, false, false), "ordinary frame timing permits blur");
    expect(!blur_sample_valid(10.f, true, false), "camera cuts must bypass blur");
    expect(!blur_sample_valid(10.f, false, true), "uncorrected jitter must bypass blur");
    for (float timing : {0.f, -1.f, 251.f, infinity, nan})
        expect(!blur_sample_valid(timing, false, false), "invalid or stalled timing must bypass blur");
    expect(!fsr_render_pixel_scale({infinity, 1.0f}, render, render), "infinite dispatch scales must be rejected");
    expect(!fsr_render_pixel_scale({1.0f, nan}, render, render), "NaN dispatch scales must be rejected");
    expect(!fsr_render_pixel_scale({max_float, 1.0f}, render, {1, 720}), "overflowing scale must be rejected");
    expect(!fsr_render_pixel_scale({1.0f, 1.0f}, {0, 720}, render), "empty render extents must be rejected");
    expect(!fsr_render_pixel_scale({1.0f, 1.0f}, render, {1280, 0}), "empty motion extents must be rejected");
}
