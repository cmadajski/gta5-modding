add_executable(native_motion_blur_gpu_test gpu_test.cpp "${PROJECT_SOURCE_DIR}/src/native_blur.cpp"
    "${PROJECT_SOURCE_DIR}/src/wheel_mask.cpp")
target_compile_features(native_motion_blur_gpu_test PRIVATE cxx_std_20)
target_compile_definitions(native_motion_blur_gpu_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(native_motion_blur_gpu_test PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_BINARY_DIR}/generated"
)
target_include_directories(native_motion_blur_gpu_test SYSTEM PRIVATE "${reshade_SOURCE_DIR}/include")
target_link_libraries(native_motion_blur_gpu_test PRIVATE d3d12 user32)
if(MINGW)
    target_link_libraries(native_motion_blur_gpu_test PRIVATE d3dcompiler_47)
    target_link_options(native_motion_blur_gpu_test PRIVATE -static)
    if(NOT CMAKE_HOST_WIN32)
        target_include_directories(native_motion_blur_gpu_test SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/cmake/mingw-compat")
    endif()
else()
    target_link_libraries(native_motion_blur_gpu_test PRIVATE d3dcompiler)
endif()

if(NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR)
    add_test(NAME native_motion_blur_gpu_test COMMAND native_motion_blur_gpu_test)
    set_tests_properties(native_motion_blur_gpu_test PROPERTIES SKIP_RETURN_CODE 77)
endif()
