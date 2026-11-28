#include "wheel_motion_history.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>

namespace {

using gtav::wheel_motion::FitLimits;
using gtav::wheel_motion::Matrix;
using gtav::wheel_motion::PreviousFit;
using gtav::wheel_motion::fit_previous_wvp;

constexpr std::uint32_t width = 1920, height = 1080;

void expect(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool same_bits(const Matrix &left, const Matrix &right)
{
    using Bytes = std::array<std::byte, sizeof(Matrix)>;
    return std::bit_cast<Bytes>(left) == std::bit_cast<Bytes>(right);
}

void expect_stock(PreviousFit result, const Matrix &stock, const char *message)
{
    expect(!result.corrected && result.roll_radians == 0 && same_bits(result.previous_wvp, stock), message);
}

Matrix multiply(const Matrix &left, const Matrix &right)
{
    Matrix result{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            for (std::size_t i = 0; i < 4; ++i)
                result[4 * row + column] += left[4 * row + i] * right[4 * i + column];
    return result;
}

Matrix roll(double radians)
{
    const auto c = static_cast<float>(std::cos(radians));
    const auto s = static_cast<float>(std::sin(radians));
    return {
        1, 0, 0, 0,
        0, c, s, 0,
        0, -s, c, 0,
        0, 0, 0, 1,
    };
}

Matrix clip_shift(Matrix matrix, double x_pixels, double y_pixels)
{
    const double dx = 2 * x_pixels / width;
    const double dy = -2 * y_pixels / height;
    for (std::size_t row = 0; row < matrix.size(); row += 4) {
        matrix[row] = static_cast<float>(matrix[row] + dx * matrix[row + 3]);
        matrix[row + 1] = static_cast<float>(matrix[row + 1] + dy * matrix[row + 3]);
    }
    return matrix;
}

void test_perspective(const Matrix &stock)
{
    FitLimits strict;
    strict.max_basis_relative_error = 0.000001;
    for (const double expected_angle : {0.43, -0.61}) {
        const auto expected = multiply(roll(expected_angle), stock);
        const auto historical = clip_shift(expected, 0.24, -0.33);
        const auto result = fit_previous_wvp(stock, historical, width, height, strict);
        expect(result.corrected && std::abs(result.roll_radians - expected_angle) < 0.000001,
               "jitter-aligned perspective matrices must recover the signed roll");
        for (std::size_t i = 0; i < stock.size(); ++i) {
            if (i < 4 || i >= 12)
                expect(std::bit_cast<std::uint32_t>(result.previous_wvp[i]) == std::bit_cast<std::uint32_t>(stock[i]),
                       "the correction must retain the exact stock axle and translation rows");
            else
                expect(std::abs(result.previous_wvp[i] - expected[i]) < 0.000001f,
                       "the correction must rotate the stock basis without copying observed jitter");
        }
    }
}

void test_measured_limits(const Matrix &stock)
{
    constexpr double angle = 2.1;
    const auto historical = clip_shift(multiply(roll(angle), stock), 1.2, -0.5);
    FitLimits limits{.max_origin_delta_pixels = 2, .max_roll_radians = std::numbers::pi_v<double>};
    const auto result = fit_previous_wvp(stock, historical, width, height, limits);
    expect(result.corrected && std::abs(result.roll_radians - angle) < 0.000001,
           "explicit limits must allow 1.3 pixels of jitter and a rotation beyond 90 degrees");

    auto tighter = limits;
    tighter.max_origin_delta_pixels = 0.75;
    expect_stock(fit_previous_wvp(stock, historical, width, height, tighter), stock,
                 "the configured tighter origin bound must reject the same history");
    tighter = limits;
    tighter.max_roll_radians = std::numbers::pi_v<double> / 2;
    expect_stock(fit_previous_wvp(stock, historical, width, height, tighter), stock,
                 "the configured tighter roll bound must reject the same history");
    expect_stock(fit_previous_wvp(stock, clip_shift(multiply(roll(angle), stock), 2.05, 0), width, height, limits),
                 stock, "the two-pixel bound must still reject larger changes");
}

void test_mammoth_capture()
{
    // Recorded CPU matrices for the same 14,388-index rigid wheel draw in
    // benchmark 02, dispatch 9646 current and dispatch 9647 stock previous.
    constexpr Matrix historical{
        0.837107062f, 0.02372366f, 7.14884095e-07f, -0.0476582199f,
        0.0156128118f, -2.07598686f, 5.47549917e-07f, -0.0365027785f,
        0.0807690248f, -0.0755520985f, -1.44877658e-05f, 0.965836525f,
        -1.21168208f, -4.4374404f, 0.149901614f, 6.70896912f,
    };
    constexpr Matrix stock{
        0.837103486f, 0.0261942372f, 7.16474176e-07f, -0.0477642268f,
        -0.0738171563f, -0.428280771f, 1.41893852e-05f, -0.945944726f,
        0.0364737362f, -2.03267455f, -2.97524525e-06f, 0.198346689f,
        -1.21561468f, -4.44520569f, 0.149901614f, 6.70896912f,
    };
    constexpr FitLimits limits{.max_origin_delta_pixels = 2, .max_roll_radians = std::numbers::pi_v<double>};
    const auto result = fit_previous_wvp(stock, historical, 1706, 960, limits);
    constexpr double degrees_per_radian = 180 / std::numbers::pi_v<double>;
    expect(result.corrected && std::abs(result.roll_radians * degrees_per_radian - 75.986354) < 0.00001,
           "recorded Mammoth matrices must recover the measured rigid clip-space rotation");
    for (std::size_t i : {0U, 1U, 2U, 3U, 12U, 13U, 14U, 15U})
        expect(std::bit_cast<std::uint32_t>(result.previous_wvp[i]) == std::bit_cast<std::uint32_t>(stock[i]),
               "captured matrix correction must retain the actual native axle and center rows");
}

void test_rejection(const Matrix &stock)
{
    const FitLimits limits;
    const auto rotated = multiply(roll(0.43), stock);
    const auto rejected = [&](const Matrix &prior, const char *message) {
        expect_stock(fit_previous_wvp(stock, prior, width, height, limits), stock, message);
    };

    for (const auto &prior : {clip_shift(stock, 0.24, -0.33), multiply(roll(0.0001), stock)}) {
        const auto fit = fit_previous_wvp(stock, prior, width, height, limits);
        expect_stock(fit, stock, "stationary and tiny rolls must leave the stock matrix bitwise unchanged");
        expect(fit.stationary, "classify a validated fit below the roll threshold as stationary");
    }
    const auto orthographic = roll(0);
    auto scaled = orthographic;
    // Opposing scale errors cancel in the least-squares cosine, yielding
    // an apparent identity roll whose two radial basis rows disagree.
    scaled[5] = 1.03f;
    scaled[10] = 0.97f;
    const auto scaled_fit = fit_previous_wvp(orthographic, scaled, width, height, limits);
    expect_stock(scaled_fit, orthographic, "a nonrigid zero-angle fit must retain the stock matrix");
    expect(!scaled_fit.stationary, "exclude opposing radial scale errors from the stationary count");
    rejected(clip_shift(rotated, 0.54, 0.54), "radial origin drift beyond the pixel tolerance must reject");
    rejected(multiply(roll(1.8), stock), "a roll above the configured maximum must reject");

    auto prior = rotated;
    prior[14] += 0.02f;
    rejected(prior, "different projected origin depth must reject");
    prior = rotated;
    for (std::size_t i = 12; i < prior.size(); ++i)
        prior[i] *= 1.02f;
    rejected(prior, "different clip W with the same normalized origin must reject");
    prior = rotated;
    prior[15] = 0;
    rejected(prior, "a wheel on the projection plane must reject");
    prior[15] = -1;
    rejected(prior, "a wheel behind the projection plane must reject");

    const Matrix changed_axle{
        0.8f, 0.6f, 0, 0,
        -0.6f, 0.8f, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    rejected(multiply(changed_axle, rotated), "a view or rotation mismatch that changes the axle must reject");
    prior = rotated;
    for (std::size_t i = 0; i < 4; ++i)
        prior[4 + i] += 0.05f * prior[i];
    rejected(prior, "a sheared plane must reject");
    prior = rotated;
    for (std::size_t i = 4; i < 12; ++i)
        prior[i] *= 1.03f;
    rejected(prior, "a scaled plane must reject");
    prior = rotated;
    for (std::size_t i = 8; i < 12; ++i)
        prior[i] *= -1;
    rejected(prior, "a reflected plane must reject");
    prior = rotated;
    for (std::size_t i = 0; i < 4; ++i)
        prior[4 + i] = prior[8 + i];
    rejected(prior, "a singular projected basis must reject");

    auto almost_singular = stock;
    for (std::size_t i = 0; i < 4; ++i)
        almost_singular[i] *= 1e-8f;
    expect_stock(fit_previous_wvp(almost_singular, multiply(roll(0.43), almost_singular), width, height, limits),
                 almost_singular, "a nearly collapsed axle must reject even when the roll is exact");

    prior = rotated;
    prior[5] = std::numeric_limits<float>::infinity();
    rejected(prior, "nonfinite history must reject");
    auto bad_stock = stock;
    bad_stock[1] = -0.0f;
    bad_stock[5] = std::bit_cast<float>(UINT32_C(0x7fc01234));
    expect_stock(fit_previous_wvp(bad_stock, rotated, width, height, limits), bad_stock,
                 "rejection must preserve even NaN payload and signed zero in the stock matrix");

    auto invalid_limits = limits;
    invalid_limits.max_clip_w_relative_delta = std::numeric_limits<double>::quiet_NaN();
    expect_stock(fit_previous_wvp(stock, rotated, width, height, invalid_limits), stock,
                 "nonfinite limits must return the stock matrix");
    auto smaller_limits = limits;
    smaller_limits.max_roll_radians = 0.3;
    expect_stock(fit_previous_wvp(stock, rotated, width, height, smaller_limits), stock,
                 "the caller's maximum roll must govern eligibility");
    expect_stock(fit_previous_wvp(stock, rotated, 0, height, limits), stock,
                 "invalid viewport must return the stock matrix");
}

} // namespace

int main()
{
    const Matrix world_view{
        0.8f, -0.f, -0.6f, 0,
        0.3f, 0.8660254f, 0.4f, 0,
        0.5196152f, -0.5f, 0.6928203f, 0,
        0.2f, -0.35f, 14.f, 1,
    };
    const Matrix perspective{
        1.3f, 0, 0, 0,
        0, 2.3f, 0, 0,
        0, 0, 1.0001f, 1,
        0, 0, -0.10001f, 0,
    };
    const auto stock = multiply(world_view, perspective);
    test_perspective(stock);
    test_measured_limits(stock);
    test_mammoth_capture();
    test_rejection(stock);
}
