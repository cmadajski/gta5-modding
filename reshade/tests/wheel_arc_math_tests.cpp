#include "wheel_arc_math.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>

namespace {

using namespace gtav::wheel_arc;

constexpr double pi = std::numbers::pi_v<double>;

void expect(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void expect_uv(std::optional<Vec2> actual, Vec2 expected, const char *message)
{
    expect(actual && std::hypot(actual->x - expected.x, actual->y - expected.y) < 1e-7, message);
}

void expect_position(std::optional<Vec3> actual, Vec3 expected, const char *message)
{
    expect(actual && std::hypot(actual->x - expected.x, actual->y - expected.y, actual->z - expected.z) < 1e-5, message);
}

constexpr Matrix identity{
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
};

struct Projection {
    Vec2 uv;
    double depth;
};

Projection project_point(const Matrix &m, Vec3 q)
{
    const double x = q.x*m[0] + q.y*m[4] + q.z*m[8] + m[12];
    const double y = q.x*m[1] + q.y*m[5] + q.z*m[9] + m[13];
    const double z = q.x*m[2] + q.y*m[6] + q.z*m[10] + m[14];
    const double w = q.x*m[3] + q.y*m[7] + q.z*m[11] + m[15];
    return {{(1 + x/w) / 2, (1 - y/w) / 2}, z/w};
}

Matrix rotate_basis(Matrix original, double radians)
{
    auto rotated = original;
    for (std::size_t i = 0; i < 4; ++i) {
        rotated[4 + i] = static_cast<float>(std::cos(radians) * original[4 + i] + std::sin(radians) * original[8 + i]);
        rotated[8 + i] = static_cast<float>(std::cos(radians) * original[8 + i] - std::sin(radians) * original[4 + i]);
    }
    return rotated;
}

void test_circle()
{
    constexpr std::uint32_t width = 800, height = 300;
    constexpr Matrix orthographic{
        0, 0, .25f, 0,
        2.f / width, 0, 0, 0,
        0, -2.f / height, 0, 0,
        0, 0, 0.5f, 1,
    };
    constexpr Vec3 point{.12, 40, 0};
    constexpr double quarter_turn = pi / 2;
    const auto center = project_point(orthographic, point);
    const auto inverse = invert_current_wvp(orthographic);
    expect(inverse.has_value(), "orthographic wheel projection must invert");
    expect_position(reconstruct_local(*inverse, center.uv, center.depth), point, "orthographic inverse must preserve tire width");

    const auto halfway = project_arc(orthographic, orthographic, point, quarter_turn, .5);
    const double component = 40 / std::sqrt(2.0);
    expect_uv(halfway, {.5 + component / width, .5 + component / height}, "halfway tap must be on the circle at 45 degrees");
    const auto mirrored = project_arc(orthographic, orthographic, point, quarter_turn, -.5);
    expect_uv(mirrored, {.5 + component / width, .5 - component / height}, "negative time must mirror the centered shutter");
    const auto previous = project_arc(orthographic, orthographic, point, quarter_turn, 1);
    expect_uv(previous, {.5, .5 + 40.0 / height}, "end of interval must align with the rotated previous endpoint");
    const double radius = std::hypot((halfway->x - .5)*width, (halfway->y - .5)*height);
    const double chord_radius = std::hypot(((center.uv.x + previous->x)/2 - .5)*width,
                                           ((center.uv.y + previous->y)/2 - .5)*height);
    expect(std::abs(radius - 40) < 1e-5 && chord_radius < 29, "the curved tap must retain radius while the linear midpoint cuts inward");

    expect(!bounded_arc_tap(orthographic, orthographic, point, quarter_turn, .5, center.uv, width, height, 30),
           "a 30 px cap must reject the 30.61 px tap without moving it off the arc");
    expect_uv(bounded_arc_tap(orthographic, orthographic, point, quarter_turn, .5, center.uv, width, height, 31),
              *halfway, "a wider pixel cap must retain the same curved location");
    expect(!bounded_arc_tap(orthographic, orthographic, point, quarter_turn, .5, center.uv, width*2, height*2, 31),
           "the radius cap must use upscaled output pixels");
    expect_uv(bounded_arc_tap(orthographic, orthographic, point, quarter_turn, .5, center.uv, width*2, height*2, 62),
              *halfway, "upscaling must preserve the normalized arc trajectory");

    auto translated = orthographic;
    translated[12] = .1f;
    translated[13] = -.2f;
    expect_uv(project_arc(orthographic, translated, point, 0, .25),
              {center.uv.x + .0125, center.uv.y + .025}, "zero roll must reduce to the camera/chassis translation");
}

// Recorded perspective WVP from the Mammoth rigid wheel at benchmark dispatch
// 9646. Its nonzero, varying W exercises projection across the tire width.
constexpr Matrix perspective{
    0.837107062f, 0.02372366f, 7.14884095e-07f, -0.0476582199f,
    0.0156128118f, -2.07598686f, 5.47549917e-07f, -0.0365027785f,
    0.0807690248f, -0.0755520985f, -1.44877658e-05f, 0.965836525f,
    -1.21168208f, -4.4374404f, 0.149901614f, 6.70896912f,
};

void test_perspective()
{
    auto stock = perspective;
    stock[0] += .025f;
    stock[12] += .3f;
    stock[13] += .7f;
    stock[15] += .5f;
    constexpr double roll = -.84;
    const auto corrected = rotate_basis(stock, roll);
    const auto inverse = invert_current_wvp(perspective);
    expect(inverse.has_value(), "captured perspective WVP must invert");

    const Vec3 a{-.35, .55, -.21}, b{.45, a.y, a.z};
    for (const auto q : {a, b}) {
        const auto projected = project_point(perspective, q);
        expect_position(reconstruct_local(*inverse, projected.uv, projected.depth), q,
                        "perspective inverse must reconstruct all three wheel-local coordinates");
        expect_uv(project_arc(perspective, stock, q, roll, 0), projected.uv, "perspective arc center must match current projection");
        expect_uv(project_arc(perspective, stock, q, roll, 1), project_point(corrected, q).uv,
                  "perspective arc must preserve the native corrected endpoint");

        constexpr double t = .3;
        Matrix intermediate;
        for (std::size_t i = 0; i < intermediate.size(); ++i)
            intermediate[i] = static_cast<float>((1 - t) * perspective[i] + t * stock[i]);
        expect_uv(project_arc(perspective, stock, q, roll, t), project_point(rotate_basis(intermediate, roll*t), q).uv,
                  "a perspective tap must rotate on the locally projected path before dividing by clip W");
    }
    const auto arc_a = project_arc(perspective, stock, a, roll, .25);
    const auto arc_b = project_arc(perspective, stock, b, roll, .25);
    expect(arc_a && arc_b && std::abs(arc_a->x - arc_b->x) > .01,
           "two tread points at different local X must follow different projected arcs");

    WheelGpuConstants gpu;
    gpu.world = identity;
    gpu.current = perspective;
    gpu.stock_previous = stock;
    gpu.corrected_previous = corrected;
    gpu.frame_id = 9646;
    gpu.canonical_id = 77;
    gpu.render_width = 1706;
    gpu.render_height = 960;
    gpu.pool_index = 12;
    expect(prepare_constants(gpu, roll) && gpu.valid == 1 && gpu.inverse_current == *inverse &&
           std::abs(gpu.roll - roll) < 1e-6, "valid constants must carry an inverse and signed roll");
    expect(gpu.world == identity && gpu.current == perspective && gpu.corrected_previous == corrected &&
           gpu.stock_previous == stock && gpu.pool_index == 12 && gpu.canonical_id == 77,
           "preparing arc constants must preserve native shader and identity fields");
    expect(!prepare_constants(gpu, 0) && gpu.valid == 0 && gpu.roll == 0 && gpu.inverse_current == Matrix{} &&
           gpu.corrected_previous == corrected, "stationary/no-arc preparation must clear metadata and retain native results");
    gpu.frame_id = 0;
    expect(!prepare_constants(gpu, roll) && gpu.valid == 0, "zero frame tag must leave arc disabled");
}

void test_invalid_projection()
{
    auto matrix = identity;
    matrix[0] = 1e-10f;
    expect(!invert_current_wvp(matrix), "ill-conditioned matrix must reject even when its float inverse is exactly representable");
    matrix[0] = 0;
    matrix[1] = 1;
    expect(!invert_current_wvp(matrix), "copied basis rows must reject as singular");
    matrix = identity;
    matrix[3] = std::numeric_limits<float>::quiet_NaN();
    expect(!invert_current_wvp(matrix), "nonfinite clip transform must not produce an inverse");
    expect(!project_arc(identity, matrix, {.1,.2,.3}, .4, 0), "arc data with a nonfinite source projection must reject");

    matrix = identity;
    matrix[15] = -1;
    expect(!project_arc(matrix, matrix, {.1,.2,.3}, .4, .25), "arc cannot project a point behind the camera");
    expect(!reconstruct_local(matrix, {.5,.5}, .5), "inverse reconstruction must reject nonpositive homogeneous W");
    matrix[15] = 1e-9f;
    expect(!project_arc(matrix, matrix, {.1,.2,.3}, .4, .25), "arc must reject the projection-plane neighborhood");
    expect(!reconstruct_local(identity, {.5,.5}, -0.1) && !reconstruct_local(identity, {.5,.5}, 1.1) &&
           !reconstruct_local(identity, {-0.2,.5}, .5), "out-of-viewport samples or invalid D3D depth must reject");

    expect(!bounded_arc_tap(identity, identity, {-1,0,0}, .4, 0, {.5,.5}, 800, 300, 1000),
           "a tap on the texture edge must reject before sampling");
    expect(!bounded_arc_tap(identity, identity, {0,0,0}, .4, 0, {.5,.5}, 0, 300, 2), "zero output extent must reject");
    expect(!bounded_arc_tap(identity, identity, {0,0,0}, .4, 0, {.5,.5}, 800, 300, -1), "negative radius must reject");
}

} // namespace

int main()
{
    test_circle();
    test_perspective();
    test_invalid_projection();
}
