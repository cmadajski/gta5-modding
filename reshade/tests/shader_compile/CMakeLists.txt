set(reshade_compiler_source "${reshade_SOURCE_DIR}/source")

add_executable(native_motion_shader_compile
    shader_compile.cpp
    "${reshade_compiler_source}/effect_codegen_dxbc.cpp"
    "${reshade_compiler_source}/effect_expression.cpp"
    "${reshade_compiler_source}/effect_lexer.cpp"
    "${reshade_compiler_source}/effect_parser_exp.cpp"
    "${reshade_compiler_source}/effect_parser_stmt.cpp"
    "${reshade_compiler_source}/effect_preprocessor.cpp"
    "${reshade_compiler_source}/effect_symbol_table.cpp"
)
set_target_properties(native_motion_shader_compile PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
target_include_directories(native_motion_shader_compile SYSTEM PRIVATE
    "${reshade_compiler_source}"
    "${reshade_SOURCE_DIR}/include"
)

if(MINGW)
    # Keep the optional upstream compiler build quick. The harness passes
    # ReShade's shader optimization flags to D3DCompile separately.
    target_compile_options(native_motion_shader_compile PRIVATE -include share.h -O0)
    target_link_options(native_motion_shader_compile PRIVATE -static)
    target_link_libraries(native_motion_shader_compile PRIVATE d3dcompiler_47)
else()
    target_link_libraries(native_motion_shader_compile PRIVATE d3dcompiler)
endif()

configure_file("${PROJECT_SOURCE_DIR}/shaders/NativeMotionVectors.fx" NativeMotionVectors.fx COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/shaders/NativeMotionBlur.fx" NativeMotionBlur.fx COPYONLY)

if(NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR)
    add_test(NAME native_motion_shader_compile COMMAND native_motion_shader_compile NativeMotionVectors.fx)
    add_test(NAME native_motion_blur_shader_compile COMMAND native_motion_shader_compile NativeMotionBlur.fx)
endif()
