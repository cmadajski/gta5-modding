# Shader compiler smoke test

These tests use the pinned ReShade compiler sources and `D3DCompiler_47` to compile `NativeMotionVectors.fx` and `NativeMotionBlur.fx` to D3D12 shader model 5.1 bytecode in Standard and Performance Modes. They also check that the add-on's metadata uniforms, including the blur's frame interval, stay writable in both modes. The tests run without the game or a graphics device.

Add `-DNATIVE_MOTION_BUILD_SHADER_SMOKE_TEST=ON` when configuring the Windows x64 build. Build and run the test from the `reshade/` source directory (`cd reshade` from the repository root).

```sh
cmake --build build-windows --config Release --target native_motion_shader_compile
ctest --test-dir build-windows -C Release -R '^native_motion(_blur)?_shader_compile$' --output-on-failure
```

On Linux, use the project's LLVM-MinGW cross-build. To run through CTest, configure with `-DCMAKE_CROSSCOMPILING_EMULATOR=/absolute/path/to/wine` and use a Wine installation with a D3D shader compiler that supports shader model 5.1. The CTest process inherits `WINEPREFIX`. A direct run with Valve's Proton Experimental can use a temporary prefix and its matching compiler dependencies.

```bash
proton_files='/absolute/path/to/Proton - Experimental/files'
export WINEPREFIX="$(mktemp -d)/prefix"
export WINEDLLOVERRIDES='winemenubuilder.exe=d;mscoree=d;mshtml=d'
"$proton_files/bin/wine" wineboot -u
cp "$proton_files"/lib/vkd3d/x86_64-windows/libvkd3d{,-shader,-utils}-1.dll \
  "$WINEPREFIX/drive_c/windows/system32/"
(cd build-windows/tests/shader_compile &&
  "$proton_files/bin/wine" ./native_motion_shader_compile.exe NativeMotionVectors.fx &&
  "$proton_files/bin/wine" ./native_motion_shader_compile.exe NativeMotionBlur.fx)
```
