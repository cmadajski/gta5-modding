#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <numeric>

namespace gtav::wheel_motion {

// Native shader layout: local x * row 0 + y * row 1 + z * row 2 + row 3.
using Matrix = std::array<float, 16>;

struct FitLimits {
    // Keep the defaults conservative. Render-path callers can supply limits
    // measured from their own captures, including up to two viewport pixels.
    double max_origin_delta_pixels = 0.75;
    double max_clip_w_relative_delta = 0.001;
    double max_ndc_depth_delta = 0.0001;
    double max_basis_relative_error = 0.005;
    double max_unit_circle_error = 0.005;
    double min_normalized_projected_determinant = 0.000001;
    double min_roll_radians = 0.001;
    double max_roll_radians = std::numbers::pi_v<double> / 2;
};

struct PreviousFit {
    Matrix previous_wvp;
    bool corrected = false;
    double roll_radians = 0;
    bool stationary = false;
};

namespace detail {

using ClipRow = std::array<double, 4>;
using ClipRows = std::array<ClipRow, 4>;
using Point = std::array<double, 3>;

inline bool valid_limits(const FitLimits &limits) noexcept
{
    const std::array values{
        limits.max_origin_delta_pixels, limits.max_clip_w_relative_delta, limits.max_ndc_depth_delta,
        limits.max_basis_relative_error, limits.max_unit_circle_error, limits.min_normalized_projected_determinant,
        limits.min_roll_radians, limits.max_roll_radians,
    };
    return std::all_of(values.begin(), values.end(), [](double value) { return std::isfinite(value) && value >= 0; }) &&
           limits.max_origin_delta_pixels <= 2 && limits.max_clip_w_relative_delta < 1 &&
           limits.max_ndc_depth_delta < 1 && limits.max_basis_relative_error < 1 && limits.max_unit_circle_error < 1 &&
           limits.min_normalized_projected_determinant > 0 && limits.min_normalized_projected_determinant <= 1 &&
           limits.min_roll_radians < limits.max_roll_radians && limits.max_roll_radians <= std::numbers::pi_v<double>;
}

inline ClipRows clip_rows(const Matrix &matrix) noexcept
{
    ClipRows rows;
    for (std::size_t row = 0; row < rows.size(); ++row)
        std::copy_n(matrix.begin() + row * rows[row].size(), rows[row].size(), rows[row].begin());
    return rows;
}

inline Point ndc_origin(const ClipRow &origin) noexcept
{
    return {origin[0] / origin[3], origin[1] / origin[3], origin[2] / origin[3]};
}

// Test the rank of the projected differential at the wheel origin. Removing
// each row's W contribution makes this independent of screen-space jitter.
inline bool full_rank(const ClipRows &rows, const Point &origin, double minimum) noexcept
{
    std::array<Point, 3> axes;
    double longest = 0;
    for (std::size_t row = 0; row < axes.size(); ++row) {
        auto &axis = axes[row];
        for (std::size_t component = 0; component < axis.size(); ++component)
            axis[component] = rows[row][component] - origin[component] * rows[row][3];
        const double length = std::hypot(axis[0], axis[1], axis[2]);
        longest = std::max(longest, length);
    }
    if (!(longest > 0) || !std::isfinite(longest))
        return false;
    for (auto &axis : axes)
        for (double &component : axis)
            component /= longest;

    const auto &[a, b, c] = axes;
    const double determinant = a[0] * (b[1] * c[2] - b[2] * c[1])
                             - a[1] * (b[0] * c[2] - b[2] * c[0])
                             + a[2] * (b[0] * c[1] - b[1] * c[0]);
    return std::abs(determinant) >= minimum;
}

inline double dot(const ClipRow &left, const ClipRow &right) noexcept
{
    return std::inner_product(left.begin(), left.end(), right.begin(), 0.0);
}

inline bool rows_agree(const ClipRow &left, const ClipRow &right, double relative_tolerance) noexcept
{
    ClipRow difference;
    for (std::size_t component = 0; component < difference.size(); ++component)
        difference[component] = left[component] - right[component];
    return dot(difference, difference) <= relative_tolerance * relative_tolerance *
           std::max(dot(left, left), dot(right, right));
}

} // namespace detail

// The caller must establish unique draw/mesh identity and adjacent submitted
// frames before supplying the earlier draw's current WVP as history. Viewport
// dimensions are those used to rasterize these matrices. A rejected or
// stationary fit returns the entire stock matrix and corrected == false.
inline PreviousFit fit_previous_wvp(const Matrix &stock_previous, const Matrix &observed_prior_current,
                                    std::uint32_t viewport_width, std::uint32_t viewport_height,
                                    const FitLimits &limits) noexcept
{
    const PreviousFit unchanged{stock_previous};
    const auto finite = [](float value) { return std::isfinite(value); };
    if (viewport_width == 0 || viewport_height == 0 || !detail::valid_limits(limits) ||
        !std::all_of(stock_previous.begin(), stock_previous.end(), finite) ||
        !std::all_of(observed_prior_current.begin(), observed_prior_current.end(), finite))
        return unchanged;

    const auto stock = detail::clip_rows(stock_previous);
    auto observed = detail::clip_rows(observed_prior_current);
    const double stock_w = stock[3][3], observed_w = observed[3][3];
    if (stock_w <= 0 || observed_w <= 0 ||
        std::abs(stock_w - observed_w) > limits.max_clip_w_relative_delta * std::max(stock_w, observed_w))
        return unchanged;

    const auto stock_origin = detail::ndc_origin(stock[3]);
    const auto observed_origin = detail::ndc_origin(observed[3]);
    const double dx = stock_origin[0] - observed_origin[0];
    const double dy = stock_origin[1] - observed_origin[1];
    if (std::hypot(dx * viewport_width * 0.5, dy * viewport_height * 0.5) > limits.max_origin_delta_pixels ||
        std::abs(stock_origin[2] - observed_origin[2]) > limits.max_ndc_depth_delta ||
        !detail::full_rank(stock, stock_origin, limits.min_normalized_projected_determinant) ||
        !detail::full_rank(observed, observed_origin, limits.min_normalized_projected_determinant))
        return unchanged;

    // A screen-space jitter shift changes every row in proportion to its W.
    // Move the observed basis into the stock projection before comparing it.
    for (std::size_t row = 0; row < 3; ++row) {
        observed[row][0] += dx * observed[row][3];
        observed[row][1] += dy * observed[row][3];
    }
    if (!detail::rows_agree(stock[0], observed[0], limits.max_basis_relative_error))
        return unchanged;

    // Least squares over clip-space Y/Z rows under a local-X rotation.
    const auto &stock_y = stock[1];
    const auto &stock_z = stock[2];
    const auto &observed_y = observed[1];
    const auto &observed_z = observed[2];
    const double plane_energy = detail::dot(stock_y, stock_y) + detail::dot(stock_z, stock_z);
    const double fitted_cosine = (detail::dot(stock_y, observed_y) + detail::dot(stock_z, observed_z)) / plane_energy;
    const double fitted_sine = (detail::dot(stock_z, observed_y) - detail::dot(stock_y, observed_z)) / plane_energy;
    const double radius = std::hypot(fitted_cosine, fitted_sine);
    if (!std::isfinite(radius) || std::abs(radius - 1) > limits.max_unit_circle_error)
        return unchanged;

    const double angle = std::atan2(fitted_sine, fitted_cosine);
    if (std::abs(angle) <= limits.min_roll_radians) {
        if (detail::rows_agree(stock_y, observed_y, limits.max_basis_relative_error) &&
            detail::rows_agree(stock_z, observed_z, limits.max_basis_relative_error))
            return {.previous_wvp = stock_previous, .stationary = true};
        return unchanged;
    }
    if (std::abs(angle) > limits.max_roll_radians)
        return unchanged;

    const double cosine = fitted_cosine / radius, sine = fitted_sine / radius;
    detail::ClipRow fitted_y, fitted_z;
    for (std::size_t component = 0; component < fitted_y.size(); ++component) {
        fitted_y[component] = cosine * stock_y[component] + sine * stock_z[component];
        fitted_z[component] = -sine * stock_y[component] + cosine * stock_z[component];
    }
    if (!detail::rows_agree(fitted_y, observed_y, limits.max_basis_relative_error) ||
        !detail::rows_agree(fitted_z, observed_z, limits.max_basis_relative_error))
        return unchanged;

    constexpr double max_float = std::numeric_limits<float>::max();
    Matrix corrected = stock_previous;
    for (std::size_t component = 0; component < fitted_y.size(); ++component) {
        if (std::abs(fitted_y[component]) > max_float || std::abs(fitted_z[component]) > max_float)
            return unchanged;
        corrected[4 + component] = static_cast<float>(fitted_y[component]);
        corrected[8 + component] = static_cast<float>(fitted_z[component]);
    }
    return {corrected, true, angle};
}

} // namespace gtav::wheel_motion
