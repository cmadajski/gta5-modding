find_program(NATIVE_MOTION_DXC NAMES dxc NO_CMAKE_FIND_ROOT_PATH)
if(NOT NATIVE_MOTION_DXC)
    message(STATUS "The wheel runtime GPU test needs DXC. Set NATIVE_MOTION_DXC to the host compiler.")
    return()
endif()

set(wheel_runtime_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/fixture.hlsl")
set(wheel_runtime_vs "wheel-runtime-fixture-vs.dxil")
set(wheel_runtime_ps "wheel-runtime-fixture-ps.dxil")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${wheel_runtime_vs}" "${CMAKE_CURRENT_BINARY_DIR}/${wheel_runtime_ps}"
    COMMAND "${NATIVE_MOTION_DXC}" -T vs_6_0 -E vertex -Fo "${wheel_runtime_vs}" "${wheel_runtime_hlsl}"
    COMMAND "${NATIVE_MOTION_DXC}" -T ps_6_0 -E pixel -Fo "${wheel_runtime_ps}" "${wheel_runtime_hlsl}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS "${wheel_runtime_hlsl}"
    VERBATIM
)
add_custom_target(wheel_runtime_fixture_shaders DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/${wheel_runtime_vs}"
    "${CMAKE_CURRENT_BINARY_DIR}/${wheel_runtime_ps}"
)

# The GPU fixture supplies its own synthetic-pipeline identity lookup.
add_executable(wheel_runtime_gpu_test gpu_test.cpp
    "${PROJECT_SOURCE_DIR}/src/wheel_runtime.cpp"
    "${PROJECT_SOURCE_DIR}/src/wheel_override.cpp"
    "${PROJECT_SOURCE_DIR}/src/wheel_descriptors.cpp"
    "${PROJECT_SOURCE_DIR}/src/wheel_mask.cpp"
)
add_dependencies(wheel_runtime_gpu_test wheel_runtime_fixture_shaders)
target_compile_features(wheel_runtime_gpu_test PRIVATE cxx_std_20)
target_compile_definitions(wheel_runtime_gpu_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(wheel_runtime_gpu_test PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(wheel_runtime_gpu_test PRIVATE native_motion_minhook d3d12)
if(MINGW)
    target_link_options(wheel_runtime_gpu_test PRIVATE -static)
endif()

if(NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR)
    add_test(NAME wheel_runtime_gpu_test COMMAND wheel_runtime_gpu_test "${wheel_runtime_vs}" "${wheel_runtime_ps}")
    set_tests_properties(wheel_runtime_gpu_test PROPERTIES SKIP_RETURN_CODE 77)
endif()
