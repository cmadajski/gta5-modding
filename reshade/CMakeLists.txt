cmake_minimum_required(VERSION 3.24)
project(NativeMotionVectors LANGUAGES C CXX)

include(CTest)

option(NATIVE_MOTION_BUILD_ADDON "Build the Windows x64 ReShade add-on" ${WIN32})

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(native_motion_math_tests tests/native_motion_math_tests.cpp)
    target_compile_features(native_motion_math_tests PRIVATE cxx_std_20)
    target_include_directories(native_motion_math_tests PRIVATE include)
    add_test(NAME native_motion_math_tests COMMAND native_motion_math_tests)
    add_executable(wheel_motion_history_tests tests/wheel_motion_history_tests.cpp)
    target_compile_features(wheel_motion_history_tests PRIVATE cxx_std_20)
    target_include_directories(wheel_motion_history_tests PRIVATE include)
    add_test(NAME wheel_motion_history_tests COMMAND wheel_motion_history_tests)
    add_executable(wheel_arc_math_tests tests/wheel_arc_math_tests.cpp)
    target_compile_features(wheel_arc_math_tests PRIVATE cxx_std_20)
    target_include_directories(wheel_arc_math_tests PRIVATE include)
    add_test(NAME wheel_arc_math_tests COMMAND wheel_arc_math_tests)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import numpy; import PIL"
            RESULT_VARIABLE wheel_analysis_dependencies OUTPUT_QUIET ERROR_QUIET)
        if(wheel_analysis_dependencies EQUAL 0)
            add_test(NAME wheel_vector_analysis_tests
                COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_wheel_vectors.py")
        else()
            message(STATUS "Wheel analysis tests require NumPy and Pillow")
        endif()
    endif()
endif()

if(NATIVE_MOTION_BUILD_ADDON)
    if(NOT WIN32)
        message(FATAL_ERROR "The add-on requires a Windows x64 compiler. On Linux, use cmake/llvm-mingw-x64.cmake.")
    endif()

    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles([=[
        #if !defined(_M_X64) && !defined(__x86_64__)
        #error NativeMotionVectors requires x64.
        #endif
        int main() { return 0; }
    ]=] NATIVE_MOTION_WINDOWS_X64)
    if(NOT NATIVE_MOTION_WINDOWS_X64)
        message(FATAL_ERROR "The add-on requires the x64 Windows target. For Visual Studio, configure with -A x64.")
    endif()

    include(cmake/Dependencies.cmake)

    add_library(native_motion_minhook STATIC
        "${minhook_SOURCE_DIR}/src/buffer.c"
        "${minhook_SOURCE_DIR}/src/hook.c"
        "${minhook_SOURCE_DIR}/src/trampoline.c"
        "${minhook_SOURCE_DIR}/src/hde/hde64.c"
    )
    target_include_directories(native_motion_minhook PUBLIC "${minhook_SOURCE_DIR}/include")

    file(READ "${PROJECT_SOURCE_DIR}/shaders/NativeMotionBlurCS.hlsl" native_blur_hlsl)
    configure_file(cmake/native_blur_source.hpp.in generated/native_blur_source.hpp @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/shaders/NativeMotionBlurCS.hlsl")
    add_library(NativeMotionVectors MODULE src/addon.cpp src/fsr_probe.cpp src/native_blur.cpp
        src/wheel_trace.cpp src/wheel_override.cpp src/wheel_pipeline_hooks.cpp src/wheel_descriptors.cpp
        src/wheel_runtime.cpp src/wheel_mask.cpp)
    target_compile_features(NativeMotionVectors PRIVATE cxx_std_20)
    target_compile_definitions(NativeMotionVectors PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(NativeMotionVectors PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_include_directories(NativeMotionVectors SYSTEM PRIVATE
        "${reshade_SOURCE_DIR}/include"
        "${native_motion_fidelityfx_include}"
    )
    target_link_libraries(NativeMotionVectors PRIVATE native_motion_minhook d3d12 dxgi user32)
    if(MINGW)
        target_link_libraries(NativeMotionVectors PRIVATE d3dcompiler_47)
    else()
        target_link_libraries(NativeMotionVectors PRIVATE d3dcompiler)
    endif()
    set_target_properties(NativeMotionVectors PROPERTIES PREFIX "" SUFFIX ".addon64")

    if(MSVC)
        set_target_properties(NativeMotionVectors native_motion_minhook PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    elseif(MINGW)
        target_link_options(NativeMotionVectors PRIVATE -static)
        if(NOT CMAKE_HOST_WIN32)
            target_include_directories(NativeMotionVectors SYSTEM PRIVATE cmake/mingw-compat)
        endif()
    endif()

    option(NATIVE_MOTION_BUILD_SHADER_SMOKE_TEST "Build the ReShade FX and DXBC compiler smoke test" OFF)
    option(NATIVE_MOTION_BUILD_GPU_TEST "Build the isolated D3D12 native blur GPU regression" OFF)
    if(BUILD_TESTING)
        add_subdirectory(tests/fsr_probe)
        if(NATIVE_MOTION_BUILD_SHADER_SMOKE_TEST)
            add_subdirectory(tests/shader_compile)
            add_subdirectory(tests/native_blur_compile)
        endif()
        if(NATIVE_MOTION_BUILD_GPU_TEST)
            add_subdirectory(tests/native_blur_gpu)
            add_subdirectory(tests/wheel_override_gpu)
            add_subdirectory(tests/wheel_runtime_gpu)
            add_subdirectory(tests/wheel_arc_gpu)
        endif()
    endif()

    install(TARGETS NativeMotionVectors LIBRARY DESTINATION . RUNTIME DESTINATION .)
    install(FILES README.md DESTINATION .)
    install(FILES docs/native-motion-overlay.md DESTINATION . RENAME INSTALL.md)
    install(FILES docs/live-test-2026-11-27.md DESTINATION .)
    install(FILES docs/native-motion-overlay.md docs/modding-research.md docs/live-test-2026-11-27.md DESTINATION docs)
    install(FILES scripts/restore_local_overlay.py DESTINATION scripts)
    install(FILES scripts/analyze_prehud_captures.py scripts/analyze_wheel_vectors.py
        scripts/analyze_wheel_trace.py scripts/capture_wheel_vectors.py DESTINATION scripts)
    install(FILES presets/NativeMotionBlur.ini DESTINATION .)
    install(FILES shaders/NativeMotionVectors.fx shaders/NativeMotionBlur.fx DESTINATION reshade-shaders/Shaders)
    install(FILES shaders/NativeMotionBlurCS.hlsl DESTINATION source)
    install(FILES docs/native-motion-blur.md docs/blur-test-2026-11-27.md docs/hud-blur-test-2026-11-27.md
        docs/wheel-vector-capture.md docs/enhanced-wheel-motion.md docs/wheel-vector-test-2026-11-27.md
        docs/wheel-correction-test-2026-11-27.md DESTINATION docs)
    install(FILES tests/native_blur_gpu/README.md DESTINATION tests/native_blur_gpu)
    install(FILES tests/wheel_runtime_gpu/README.md DESTINATION tests/wheel_runtime_gpu)
    install(FILES "${minhook_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses RENAME MinHook.txt)
    install(FILES "${reshade_SOURCE_DIR}/LICENSE.md" DESTINATION licenses RENAME ReShade.md)
    install(FILES "${native_motion_fidelityfx_include}/LICENSE.txt" DESTINATION licenses RENAME AMD-FidelityFX.txt)
    if(DEFINED llvm_mingw_bin AND EXISTS "${llvm_mingw_bin}/../LICENSE.TXT")
        install(FILES "${llvm_mingw_bin}/../LICENSE.TXT" DESTINATION licenses RENAME LLVM-MinGW.txt)
    endif()
endif()
