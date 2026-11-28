#include <d3d12shader.h>
#include <d3dcompiler.h>

#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

namespace {

struct ReleaseCom {
    template <class T>
    void operator()(T *object) const { object->Release(); }
};

using Blob = std::unique_ptr<ID3DBlob, ReleaseCom>;

bool check_shader_layout(ID3DBlob &bytecode)
{
    ID3D12ShaderReflection *shader_reflection = nullptr;
    const HRESULT result = D3DReflect(bytecode.GetBufferPointer(), bytecode.GetBufferSize(),
        __uuidof(ID3D12ShaderReflection), reinterpret_cast<void **>(&shader_reflection));
    const std::unique_ptr<ID3D12ShaderReflection, ReleaseCom> reflection(shader_reflection);
    if (FAILED(result)) {
        std::cerr << "D3DReflect failed (0x" << std::hex << static_cast<unsigned long>(result) << ")\n";
        return false;
    }

    constexpr struct Binding {
        const char *name;
        D3D_SHADER_INPUT_TYPE type;
        UINT slot;
    } bindings[] = {
        {"Scene", D3D_SIT_TEXTURE, 0},
        {"Motion", D3D_SIT_TEXTURE, 1},
        {"WheelMask", D3D_SIT_TEXTURE, 2},
        {"WheelDepth", D3D_SIT_TEXTURE, 3},
        {"SceneDepth", D3D_SIT_TEXTURE, 4},
        {"WheelData", D3D_SIT_STRUCTURED, 5},
        {"Result", D3D_SIT_UAV_RWTYPED, 0},
        {"SceneSampler", D3D_SIT_SAMPLER, 0},
        {"PointSampler", D3D_SIT_SAMPLER, 1},
        {"BlurConstants", D3D_SIT_CBUFFER, 0},
    };
    D3D12_SHADER_DESC shader{};
    if (FAILED(reflection->GetDesc(&shader)) || shader.BoundResources != std::size(bindings) ||
        shader.ConstantBuffers != 2) {
        std::cerr << "Unexpected compute shader resource or cbuffer count\n";
        return false;
    }
    for (const auto &binding : bindings) {
        D3D12_SHADER_INPUT_BIND_DESC desc{};
        if (FAILED(reflection->GetResourceBindingDescByName(binding.name, &desc)) ||
            desc.Type != binding.type || desc.BindPoint != binding.slot || desc.BindCount != 1 || desc.Space != 0) {
            std::cerr << "Unexpected binding for " << binding.name << '\n';
            return false;
        }
    }

    UINT x = 0, y = 0, z = 0;
    if (reflection->GetThreadGroupSize(&x, &y, &z) != 64 || x != 8 || y != 8 || z != 1) {
        std::cerr << "Expected an 8x8x1 compute group\n";
        return false;
    }

    constexpr struct Constant {
        const char *name;
        UINT offset;
        UINT size;
        D3D_SHADER_VARIABLE_TYPE type;
    } constants[] = {
        {"outputSize", 0, 8, D3D_SVT_UINT},
        {"motionActiveSize", 8, 8, D3D_SVT_UINT},
        {"rawToUV", 16, 8, D3D_SVT_FLOAT},
        {"frameTimeMs", 24, 4, D3D_SVT_FLOAT},
        {"exposureValue", 28, 4, D3D_SVT_FLOAT},
        {"exposureMode", 32, 4, D3D_SVT_UINT},
        {"sampleCount", 36, 4, D3D_SVT_UINT},
        {"maxRadius", 40, 4, D3D_SVT_FLOAT},
        {"displayMode", 44, 4, D3D_SVT_UINT},
        {"motionTextureSize", 48, 8, D3D_SVT_UINT},
        {"wheelMaskTextureSize", 56, 8, D3D_SVT_UINT},
        {"arcEnabled", 64, 4, D3D_SVT_UINT},
        {"arcFrame", 68, 4, D3D_SVT_UINT},
        {"arcCount", 72, 4, D3D_SVT_UINT},
        {"arcReserved", 76, 4, D3D_SVT_UINT},
        {"jitterUV", 80, 8, D3D_SVT_FLOAT},
        {"sceneDepthTextureSize", 88, 8, D3D_SVT_UINT},
    };
    auto *const buffer = reflection->GetConstantBufferByName("BlurConstants");
    D3D12_SHADER_BUFFER_DESC buffer_desc{};
    if (FAILED(buffer->GetDesc(&buffer_desc)) || buffer_desc.Size != 96 ||
        buffer_desc.Variables != std::size(constants)) {
        std::cerr << "Expected 24 DWORDs of root constants\n";
        return false;
    }
    for (const auto &constant : constants) {
        auto *const variable = buffer->GetVariableByName(constant.name);
        D3D12_SHADER_VARIABLE_DESC variable_desc{};
        D3D12_SHADER_TYPE_DESC type_desc{};
        if (FAILED(variable->GetDesc(&variable_desc)) || variable_desc.StartOffset != constant.offset ||
            variable_desc.Size != constant.size || FAILED(variable->GetType()->GetDesc(&type_desc)) ||
            type_desc.Type != constant.type) {
            std::cerr << "Unexpected constant layout for " << constant.name << '\n';
            return false;
        }
    }
    return true;
}

Blob compile(const std::string &source, const char *path, UINT compile_flags)
{
    ID3DBlob *code = nullptr;
    ID3DBlob *messages = nullptr;
    const HRESULT result = D3DCompile(source.data(), source.size(), path, nullptr, nullptr, "main", "cs_5_1",
        compile_flags | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &messages);
    Blob bytecode(code);
    const Blob diagnostics(messages);
    if (diagnostics)
        std::cerr << static_cast<const char *>(diagnostics->GetBufferPointer());
    if (FAILED(result) || !bytecode || bytecode->GetBufferSize() < 4 ||
        std::memcmp(bytecode->GetBufferPointer(), "DXBC", 4) != 0) {
        std::cerr << "D3DCompile failed to produce cs_5_1 DXBC (0x" << std::hex
                  << static_cast<unsigned long>(result) << ")\n";
        return {};
    }
    if (!check_shader_layout(*bytecode))
        return {};
    return bytecode;
}

bool write_header(const char *path, ID3DBlob &bytecode)
{
    std::ofstream output(path, std::ios::binary);
    output << "// Generated from NativeMotionBlurCS.hlsl.\n#pragma once\n\n"
              "inline constexpr unsigned char NativeMotionBlurCSBytecode[] = {\n";
    const auto *const data = static_cast<const unsigned char *>(bytecode.GetBufferPointer());
    const auto size = bytecode.GetBufferSize();
    output << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) {
        if (i % 12 == 0)
            output << "    ";
        output << "0x" << std::setw(2) << static_cast<unsigned>(data[i]) << ',';
        output << ((i % 12 == 11 || i + 1 == size) ? '\n' : ' ');
    }
    output << "};\n";
    output.close();
    if (output)
        return true;
    std::cerr << "Could not write " << path << '\n';
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        std::cerr << "Usage: native_motion_blur_compile <shader.hlsl> [output.h]\n";
        return 1;
    }

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "Could not open " << argv[1] << '\n';
        return 1;
    }
    const std::string source{std::istreambuf_iterator<char>(input), {}};

    const auto optimized = compile(source, argv[1], D3DCOMPILE_OPTIMIZATION_LEVEL3);
    if (!optimized)
        return 1;
    const auto debug = compile(source, argv[1], D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION);
    if (!debug)
        return 1;

    std::cout << "Validated optimized (" << optimized->GetBufferSize() << " bytes) and debug ("
              << debug->GetBufferSize() << " bytes) cs_5_1 DXBC and root constant layout\n";
    return argc == 2 || write_header(argv[2], *optimized) ? 0 : 1;
}
