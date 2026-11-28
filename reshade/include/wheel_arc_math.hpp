#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <type_traits>
#include <utility>

#include "wheel_motion_history.hpp"

namespace gtav::wheel_arc {

using wheel_motion::Matrix;

// The first 192 bytes are the existing native b11 matrices. The remaining
// bytes are read by the arc pass. Keep this layout in sync with HLSL.
struct alignas(16) WheelGpuConstants {
    Matrix world{};
    Matrix current{};
    Matrix corrected_previous{};
    Matrix stock_previous{};
    Matrix inverse_current{};
    float roll = 0;
    std::uint32_t frame_id = 0, canonical_id = 0, valid = 0;
    std::uint32_t render_width = 0, render_height = 0, pool_index = 0, reserved = 0;
    std::array<float, 40> padding{};
};

static_assert(std::is_standard_layout_v<WheelGpuConstants> && std::is_trivially_copyable_v<WheelGpuConstants>);
static_assert(sizeof(WheelGpuConstants) == 512);
static_assert(offsetof(WheelGpuConstants, world) == 0);
static_assert(offsetof(WheelGpuConstants, current) == 64);
static_assert(offsetof(WheelGpuConstants, corrected_previous) == 128);
static_assert(offsetof(WheelGpuConstants, stock_previous) == 192);
static_assert(offsetof(WheelGpuConstants, inverse_current) == 256);
static_assert(offsetof(WheelGpuConstants, roll) == 320);
static_assert(offsetof(WheelGpuConstants, frame_id) == 324);
static_assert(offsetof(WheelGpuConstants, canonical_id) == 328);
static_assert(offsetof(WheelGpuConstants, valid) == 332);
static_assert(offsetof(WheelGpuConstants, render_width) == 336);
static_assert(offsetof(WheelGpuConstants, render_height) == 340);
static_assert(offsetof(WheelGpuConstants, pool_index) == 344);
static_assert(offsetof(WheelGpuConstants, reserved) == 348);
static_assert(offsetof(WheelGpuConstants, padding) == 352);

struct Vec2 { double x, y; };
struct Vec3 { double x, y, z; };

namespace detail {

constexpr double min_positive_w = 0.0000001;
using Homogeneous = std::array<double, 4>;

template <typename T, std::size_t N>
inline bool finite(const std::array<T, N> &values) noexcept
{
    return std::all_of(values.begin(), values.end(), [](T value) { return std::isfinite(value); });
}

inline Homogeneous transform(const Homogeneous &point, const Matrix &matrix) noexcept
{
    Homogeneous result{};
    for (std::size_t row = 0; row < point.size(); ++row)
        for (std::size_t column = 0; column < result.size(); ++column)
            result[column] += point[row] * matrix[4 * row + column];
    return result;
}

inline double infinity_norm(const Matrix &matrix) noexcept
{
    double largest = 0;
    for (std::size_t row = 0; row < 4; ++row) {
        double sum = 0;
        for (std::size_t column = 0; column < 4; ++column)
            sum += std::abs(static_cast<double>(matrix[4 * row + column]));
        largest = std::max(largest, sum);
    }
    return largest;
}

inline bool close_to_identity(const Matrix &a, const Matrix &b) noexcept
{
    constexpr double max_residual = 0.001;
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column) {
            double value = 0;
            for (std::size_t i = 0; i < 4; ++i)
                value += static_cast<double>(a[4 * row + i]) * b[4 * i + column];
            if (std::abs(value - (row == column ? 1.0 : 0.0)) > max_residual)
                return false;
        }
    return true;
}

} // namespace detail

// Invert in double precision with scaled pivoting, then verify the actual
// float inverse that the GPU will read. Strongly ill-conditioned projections
// amplify quantization and depth error too much for a useful arc.
inline std::optional<Matrix> invert_current_wvp(const Matrix &current) noexcept
{
    if (!detail::finite(current))
        return {};
    std::array<std::array<double, 8>, 4> system{};
    std::array<double, 4> scales{};
    for (std::size_t row = 0; row < system.size(); ++row) {
        for (std::size_t column = 0; column < system.size(); ++column) {
            const double value = current[row * 4 + column];
            system[row][column] = value;
            scales[row] = std::max(scales[row], std::abs(value));
        }
        if (scales[row] == 0)
            return {};
        system[row][4 + row] = 1;
    }

    for (std::size_t column = 0; column < system.size(); ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < system.size(); ++row)
            if (std::abs(system[row][column]) / scales[row] > std::abs(system[pivot][column]) / scales[pivot])
                pivot = row;
        if (std::abs(system[pivot][column]) <= 1e-12 * scales[pivot])
            return {};
        std::swap(system[column], system[pivot]);
        std::swap(scales[column], scales[pivot]);
        const double divisor = system[column][column];
        for (double &value : system[column])
            value /= divisor;
        for (std::size_t row = 0; row < system.size(); ++row) {
            if (row == column)
                continue;
            const double scale = system[row][column];
            for (std::size_t i = 0; i < system[row].size(); ++i)
                system[row][i] -= scale * system[column][i];
        }
    }

    Matrix inverse{};
    constexpr double max_float = std::numeric_limits<float>::max();
    for (std::size_t row = 0; row < system.size(); ++row)
        for (std::size_t column = 0; column < system.size(); ++column) {
            const auto entry = system[row][4 + column];
            if (!std::isfinite(entry) || std::abs(entry) > max_float)
                return {};
            inverse[row * 4 + column] = static_cast<float>(entry);
        }
    constexpr double max_condition = 1e8;
    if (detail::infinity_norm(current) * detail::infinity_norm(inverse) > max_condition ||
        !detail::close_to_identity(current, inverse) || !detail::close_to_identity(inverse, current))
        return {};
    return inverse;
}

// Populate arc metadata only. Stock drawing and the corrected motion output
// keep their original prefix even if an arc cannot be sampled.
inline bool prepare_constants(WheelGpuConstants &constants, double roll_radians) noexcept
{
    constants.roll = 0;
    constants.valid = 0;
    constants.inverse_current = {};
    if (constants.frame_id == 0 || constants.canonical_id == 0 || constants.render_width == 0 || constants.render_height == 0 ||
        !std::isfinite(roll_radians) || std::abs(roll_radians) > std::numbers::pi_v<double> ||
        !detail::finite(constants.world) || !detail::finite(constants.corrected_previous) || !detail::finite(constants.stock_previous))
        return false;
    const auto gpu_roll = static_cast<float>(roll_radians);
    if (gpu_roll == 0)
        return false;
    const auto inverse = invert_current_wvp(constants.current);
    if (!inverse)
        return false;
    constants.inverse_current = *inverse;
    constants.roll = gpu_roll;
    constants.valid = 1;
    return true;
}

// D3D screen UV uses Y down, and the depth must be NDC Z in [0, 1]. Use
// the sample's own viewport/jitter convention when providing UV and depth.
inline std::optional<Vec3> reconstruct_local(const Matrix &inverse_current, Vec2 uv, double depth) noexcept
{
    if (!(uv.x >= 0 && uv.x <= 1 && uv.y >= 0 && uv.y <= 1 && depth >= 0 && depth <= 1))
        return {};
    const auto point = detail::transform({2 * uv.x - 1, 1 - 2 * uv.y, depth, 1}, inverse_current);
    if (!detail::finite(point) || point[3] <= detail::min_positive_w)
        return {};
    return Vec3{point[0] / point[3], point[1] / point[3], point[2] / point[3]};
}

// Times are in frame intervals, positive towards the previous pose. A
// shutter-centered tap uses t = normalizedTap * exposureFraction / 2.
// Rotate local YZ before projection so X stays fixed across the tire width.
// The clip interpolation approximates camera/chassis motion and preserves
// both measured endpoints. It does not recover motion inside each interval.
inline std::optional<Vec2> project_arc(const Matrix &current, const Matrix &stock_previous,
                                       Vec3 local, double roll_radians, double t) noexcept
{
    const double angle = t * roll_radians;
    if (!std::isfinite(angle) || !detail::finite(std::array{local.x, local.y, local.z}))
        return {};
    const double c = std::cos(angle), s = std::sin(angle);
    const detail::Homogeneous rotated{local.x, c * local.y - s * local.z, s * local.y + c * local.z, 1};
    const auto now = detail::transform(rotated, current);
    const auto prior = detail::transform(rotated, stock_previous);
    if (!detail::finite(now) || !detail::finite(prior))
        return {};
    const detail::Homogeneous clip{
        std::lerp(now[0], prior[0], t), std::lerp(now[1], prior[1], t),
        std::lerp(now[2], prior[2], t), std::lerp(now[3], prior[3], t),
    };
    if (!detail::finite(clip) || clip[3] <= detail::min_positive_w)
        return {};
    const Vec2 uv{0.5 + 0.5 * clip[0] / clip[3], 0.5 - 0.5 * clip[1] / clip[3]};
    return std::isfinite(uv.x) && std::isfinite(uv.y) ? std::optional{uv} : std::nullopt;
}

// Reject taps beyond the cap without moving them off the projected arc.
// Bounds use output pixel centers, independently of the render extent.
inline std::optional<Vec2> bounded_arc_tap(const Matrix &current, const Matrix &stock_previous,
                                          Vec3 local, double roll_radians, double t, Vec2 center,
                                          std::uint32_t output_width, std::uint32_t output_height,
                                          double max_radius_pixels) noexcept
{
    if (output_width == 0 || output_height == 0 || !std::isfinite(max_radius_pixels) || max_radius_pixels < 0 ||
        !(center.x >= 0 && center.x <= 1 && center.y >= 0 && center.y <= 1))
        return {};
    const auto tap = project_arc(current, stock_previous, local, roll_radians, t);
    const Vec2 half_texel{0.5 / output_width, 0.5 / output_height};
    if (!tap || !(tap->x >= half_texel.x && tap->x <= 1 - half_texel.x &&
                  tap->y >= half_texel.y && tap->y <= 1 - half_texel.y))
        return {};
    const double dx = (tap->x - center.x) * output_width;
    const double dy = (tap->y - center.y) * output_height;
    return std::hypot(dx, dy) <= max_radius_pixels ? tap : std::nullopt;
}

} // namespace gtav::wheel_arc
