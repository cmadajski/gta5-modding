# D3D12 motion blur regression test

From the `reshade/` source directory (`cd reshade` from the repository root), configure with `-DNATIVE_MOTION_BUILD_GPU_TEST=ON` and build the `native_motion_blur_gpu_test` target. Run `ctest --test-dir build-windows -R '^native_motion_blur_gpu_test$' --output-on-failure`. The test needs a D3D12 device and skips if device creation fails. It runs without GTA or ReShade.

The fixture uses a padded RGBA16F scene and a smaller motion field with moving and stationary regions. It checks the split reference, alpha, motion, the history-reset guard, and output padding. A second blur pass on the original command list samples the updated scene with a half-pixel cap, which disables blending. A separate submission reads the full destination because an inline texture-to-buffer diagnostic copy returned old pixels on the tested vkd3d-proton build.

For a Linux cross-build, configure `CMAKE_CROSSCOMPILING_EMULATOR` to Proton's `files/bin/wine`. Use a temporary prefix with Proton's matching DXVK and vkd3d-proton graphics libraries and Wine's shader compiler dependencies.

```bash
proton_files='/absolute/path/to/Proton - Experimental/files'
export WINEPREFIX="$(mktemp -d)/prefix"
WINEDLLOVERRIDES='winemenubuilder.exe=d;mscoree=d;mshtml=d' "$proton_files/bin/wine" wineboot -u
cp "$proton_files"/lib/vkd3d/x86_64-windows/libvkd3d{,-shader,-utils}-1.dll \
   "$proton_files"/lib/wine/vkd3d-proton/x86_64-windows/d3d12{,core}.dll \
   "$proton_files"/lib/wine/dxvk/x86_64-windows/dxgi.dll \
   "$WINEPREFIX/drive_c/windows/system32/"
WINEDLLOVERRIDES='d3d12=n,b;d3d12core=n,b;dxgi=n,b' \
  ctest --test-dir build-windows -R '^native_motion_blur_gpu_test$' --output-on-failure
```

The raw capture checks use RG16F motion with inactive texture padding and row-pitch alignment. They require bit-exact signed zero and NaN payload preservation, active-viewport cropping, same-list scene-before readback, and capture-slot reuse after completed GPU work and recording replacement. Capture-only mode is checked with a reset frame and must leave the scene unchanged.
