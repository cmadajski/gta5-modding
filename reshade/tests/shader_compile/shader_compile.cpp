#include "effect_codegen.hpp"
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace {

bool compile(const std::string &source, bool performance, bool is_blur)
{
    const char *mode = performance ? "Performance" : "Standard";
    // ReShade uses shader model 5.1 for D3D12 and selects O3 in Performance Mode.
    std::unique_ptr<reshadefx::codegen> backend(
        reshadefx::create_codegen_dxbc(51, false, performance, performance ? 3 : 1));
    reshadefx::parser parser;
    const bool success = parser.parse(source, backend.get());
    std::cerr << parser.errors();
    if (!success)
        return false;

    const auto &module = backend->module();
    constexpr std::array metadata_uniforms{
        "NativeMotionValid", "NativeMotionScale", "NativeMotionRenderSize",
        "NativeMotionTextureSize", "NativeMotionUVScale",
    };
    const auto check_uniform = [&](const char *name) {
        const auto uniform = std::find_if(module.uniforms.begin(), module.uniforms.end(),
            [name](const reshadefx::uniform &candidate) { return candidate.name == name; });
        if (uniform == module.uniforms.end()) {
            std::cerr << mode << " Mode is missing dynamic uniform " << name << '\n';
            return false;
        }
        return true;
    };
    if (!std::all_of(metadata_uniforms.begin(), metadata_uniforms.end(), check_uniform) ||
        (is_blur && !check_uniform("NativeMotionFrameTimeMs"))) {
        return false;
    }
    if (is_blur) {
        constexpr std::array controls{
            "NativeMotionBlurExposureMode", "NativeMotionBlurShutterAngle", "NativeMotionBlurShutterMs",
            "NativeMotionBlurSamples", "NativeMotionBlurMaxRadiusPx", "NativeMotionBlurDisplay",
        };
        if (!std::all_of(controls.begin(), controls.end(), check_uniform))
            return false;
    }

    for (const auto stage : {reshadefx::shader_type::vertex, reshadefx::shader_type::pixel}) {
        const auto entry = std::find_if(module.entry_points.begin(), module.entry_points.end(),
            [stage](const auto &candidate) { return candidate.second == stage; });
        if (entry == module.entry_points.end()) {
            std::cerr << mode << " Mode is missing a fullscreen pass entry point\n";
            return false;
        }
    }

    for (const auto &entry : module.entry_points) {
        const auto &name = entry.first;
        std::string bytecode, assembly, diagnostics;
        const bool assembled = backend->assemble_code_for_entry_point(name, bytecode, assembly, diagnostics);
        std::cerr << diagnostics;
        if (!assembled || bytecode.compare(0, 4, "DXBC") != 0) {
            std::cerr << mode << " Mode failed to compile " << name << " to DXBC\n";
            return false;
        }
        std::cout << mode << " Mode compiled " << name << " (" << bytecode.size() << " bytes DXBC)\n";
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "NativeMotionVectors.fx";
    const bool is_blur = std::filesystem::path(path).filename() == "NativeMotionBlur.fx";
    reshadefx::preprocessor preprocessor;
    preprocessor.add_macro_definition("BUFFER_WIDTH", "2560");
    preprocessor.add_macro_definition("BUFFER_HEIGHT", "1440");
    const bool success = preprocessor.append_file(path);
    std::cerr << preprocessor.errors();
    if (!success)
        return 1;

    return compile(preprocessor.output(), false, is_blur) && compile(preprocessor.output(), true, is_blur) ? 0 : 1;
}
