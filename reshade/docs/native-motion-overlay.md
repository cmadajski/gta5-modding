# Native motion vector diagnostic overlay

This first prototype targets **GTA V Enhanced in Story Mode, Steam/Proton, and the RX 7800 XT**. It captures the motion-vector texture supplied to the game's native FSR 3.1 upscaler and displays its direction and magnitude or signed X/Y components. It does not estimate optical flow and does not apply motion blur.

The Windows x64 add-on is cross-compiled on Linux. The shader compiles through Proton's DirectX compiler. Hook tests pass against a simulated FidelityFX provider, and **live capture and visualization have been verified in the GTA V Enhanced benchmark on the RX 7800 XT**. The observed source is RG16F with a pixel scale of (1, 1). See the [local test report](live-test-2026-11-27.md) for configuration, capture coverage, and remaining validation.

## Install on Steam and Proton

1. Close the game. Use Story Mode with BattlEye disabled through the Rockstar launcher setting. Keep frame generation and other graphics/upscaler injectors off for the first test.
2. Install [ReShade 6.8.0 or newer](https://reshade.me/) with add-on support for the game's DirectX 12 executable, `GTA5_Enhanced.exe`. ReShade must load as a Windows D3D12 interceptor inside Proton; a native Vulkan ReShade installation is not this add-on's target. If you already have a working compatible ReShade installation, reuse it.
3. Extract `NativeMotionVectors-win64.zip` into that executable's directory. The add-on belongs beside `ReShade.ini`; the shader belongs under the configured ReShade shader search path. The default package layout is:

   ```text
   NativeMotionVectors.addon64
   reshade-shaders/Shaders/NativeMotionVectors.fx
   licenses/...
   INSTALL.md
   ```

4. For a ReShade installation named `dxgi.dll`, use this Steam launch-option pattern:

   ```text
   WINEDLLOVERRIDES="dxgi=n,b" %command% -nobattleye
   ```

   Preserve existing launch options. If another DLL override is already present, combine its entries, for example `WINEDLLOVERRIDES="xinput1_4=n,b;dxgi=n,b"`. Do not overwrite another mod's `dxgi.dll`; use that mod's supported chaining procedure or remove the conflict for this test.

5. Start Story Mode, select **FSR 3 upscaling** in the game's graphics settings, and leave Frame Generation off. If the log reports an untracked context, change anti-aliasing/upscaling away from FSR and back. The add-on loads the existing game-local provider during D3D12 device initialization to attach before FSR context creation. If that early load fails or the add-on is attached late, recreating FSR lets the probe capture its flags correctly.
6. Open ReShade with its configured overlay key (normally Home). Enable **GTA V Native Motion Vectors** from `NativeMotionVectors.fx`. Check that ReShade lists the add-on and that the shader compiles.

No Script Hook V, ScriptHookVDotNet, OptiScaler, DLSS, or NVIDIA component is required for this FSR prototype. Build commands only produce the package; installation is a separate step.

## Read the visualization

- **Green status dot:** a fresh supported FSR sample was submitted on the expected queue and bound for this frame.
- **Red status dot:** no fresh valid sample. The original image is preserved; an old motion texture is not displayed as current data.
- **Direction and magnitude:** hue represents direction, increasing clockwise from screen-right/red; brightness represents magnitude. Zero motion is black.
- **Signed X and Y:** red represents X and green represents Y. A value of 0.5 is zero; darker and lighter represent negative and positive motion.
- **Vector units:** render pixels applies FSR's scale and resolution convention; raw RG shows sampled native values; viewport UV expresses scaled displacement relative to the viewport. These are FSR inputs before optional jitter cancellation, so the visualization may retain subpixel jitter.
- **Gain and opacity:** increase gain to reveal small vectors; lower opacity to compare vectors with the scene. The status marker has its own toggle and remains visible at zero opacity when enabled.

The vector convention at the FSR boundary is current-to-previous frame. For an object moving right across a fixed camera, its history vector should generally point left. Camera motion also contributes. This version does not isolate object-only motion by subtracting camera motion.

Metadata uniforms remain writable in ReShade Performance Mode. Disable Performance Mode when changing visualization controls.

## First in-game verification

Start at a fixed resolution with a stationary camera. Save screenshots and the `[NativeMotion]` entries from `ReShade.log` for these cases:

| Scene | Expected check |
| --- | --- |
| Static scenery and camera | Near-zero motion except animated surfaces and possible jitter. |
| Passing car, fixed camera | Distinct vectors on the moving vehicle. |
| Pedestrian animation | Limb vectors differ from torso/root motion; a moving silhouette alone does not prove skinning coverage. |
| Horizontal and vertical camera pan | Expected direction and consistent displacement scale. |
| Spinning wheel | Local rotational motion, if the game writes it. |
| Resolution change or FSR mode change | Texture recreation and resumed fresh samples. |
| FSR disabled | Red status indicator, with no stale field displayed. |

Status counters include `held_reset` (completed GPU work whose command recording is still live), `in_flight` (pending fence), `recorded` (submission not observed), `quarantined`, and allocation failures. The log records the numeric DXGI format, physical and active texture dimensions, render/output dimensions, raw and render-pixel scale, jitter, reset flags and context generation. It logs transitions between live input and missing input, with a status counter approximately every 600 presentations.

## Troubleshooting

| Log or symptom | Interpretation and next step |
| --- | --- |
| Add-on missing from ReShade | Confirm ReShade supports add-ons, the file is beside `ReShade.ini`, the x64 D3D12 ReShade loader is active, and Steam loads its proxy DLL. |
| Waiting for `amd_fidelityfx_dx12.dll` | Enable the game's FSR upscaler. Native AA, another upscaler, or a renamed/replaced FSR module may not use this entry point. |
| `unknown_context` increases | Toggle away from FSR and back after hook installation. Existing contexts are not guessed. |
| `unsupported` or unsupported `DXGI_FORMAT` | The current ABI/resource state/shape/encoding is outside this prototype's verified assumptions. Preserve the log and build information; do not interpret an empty view as zero motion. |
| `accepted` increases but `copies` stays zero | The command list was not observed by ReShade, runtime initialization is incomplete, or the dispatch is on an unsupported command-list type. |
| Different queue or presentation thread | This version intentionally stops capture for the device. Restart the game with FG and other injectors disabled. Supporting that execution path requires explicit additional synchronization. |
| `dropped` increases rapidly | All capture slots remain in flight or their command recordings have not been replaced; allocation failures or the 256 MiB texture budget can also cause drops. Try a smaller resolution and preserve the log. |
| Green indicator, unexpected directions | Use raw RG and log the scales. Check jitter and compare two consecutive rendered frames before changing sign/scale conventions. |

The hook targets the FSR 3.1-style exported API in the exact module `amd_fidelityfx_dx12.dll`. It validates descriptor types, tracked DX12 contexts, known read states and simple non-MSAA 2D textures. Public ABI descriptors have no structure-size field, so an incompatible future game/SDK update still requires a code update. Initial supported encodings are RG16F, RG32F, RGBA16F, RGBA32F and RG16_SNORM. Packed/integer/UNORM encodings are rejected.

The DLL pins itself and the provider once hooks are active. Restart the game after changing/removing the add-on; hot-unloading a detour module is not supported. To uninstall, close the game and remove `NativeMotionVectors.addon64` and `NativeMotionVectors.fx`. Leave shared ReShade files alone if other effects use them.

## Local installer and rollback

From the source checkout’s `reshade/` directory (`cd reshade` from the repository root), `scripts/install_local_overlay.py` installs an already built add-on, shader, a supplied Windows x64 ReShade DLL, and an enabled preset. It requires explicit game and backup directories, validates `GTA5_Enhanced.exe`, and records original files and SHA-256 hashes before writing replacements. `--settings-file` also saves the original game graphics configuration. Close the game first. Use the official **full add-on** ReShade build.

The installer sets **Home** for the ReShade panel, **F7** to toggle effects, and **F8** for screenshots under `NativeMotionCaptures`. It does not modify Steam launch options.

To preview rollback, use Python 3.11 or newer:

```bash
python3 scripts/restore_local_overlay.py /absolute/path/to/install-journal.json
```

Add `--apply` to perform the previewed changes. Modified files are refused; review them before using `--force` (ReShade may rewrite its own INI). `--restore-settings` explicitly restores the backed-up graphics XML. The helper preserves directories and captures and does not change Steam launch options. Remove only this overlay's `dxgi` override and `-nobattleye` option separately if restoring your previous launch setup.

## Automatic verification screenshot

Temporarily prefix the Steam launch options with `GTA_NATIVE_MOTION_CAPTURE=1` to request one screenshot after 120 fresh frames actually render through the diagnostic technique. The file is saved in ReShade's configured screenshot folder with `NativeMotion-live` in its name. This intentionally supports only direct, single-sample SDR RGBA8/BGRA8 swap chains; unsupported targets log a skipped capture. The screenshot performs a one-time readback and should be disabled for performance measurements.

A noninteractive Story Mode test can append `-benchmark -benchmarkIterations 1`. Remove these arguments after testing to return to normal startup. Keep `-nobattleye` while using the overlay in Story Mode.

## Build from source

Run the following commands from the `reshade/` source directory (`cd reshade` from the repository root).

Portable CPU tests need CMake 3.24+ and a C++20 compiler:

```bash
cmake -S . -B build-tests -DNATIVE_MOTION_BUILD_ADDON=OFF
cmake --build build-tests --parallel
ctest --test-dir build-tests --output-on-failure
```

For the Windows x64 module, use [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw/releases). The development build used the 20260826 UCRT x86-64 Linux release. Set the path to your extracted toolchain:

```bash
cmake -S . -B build-windows \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw-x64.cmake \
  -DLLVM_MINGW_ROOT=/absolute/path/to/llvm-mingw
cmake --build build-windows --parallel
cmake --install build-windows --prefix build-windows/package
```

CMake downloads SHA-256-locked ReShade 6.8.0, MinHook 1.3.4 and FidelityFX SDK 1.1.4 public headers. It links the runtime statically; it does not download or replace the game's FidelityFX DLL. `NATIVE_MOTION_DOWNLOAD_CACHE` can point to a previously prepared offline download cache. A Windows MSVC x64 build is also supported by the CMake target, though the delivered binary was built with LLVM-MinGW.

The Windows build includes a simulated-provider hook test. To run Windows CTest executables from Linux, configure `CMAKE_CROSSCOMPILING_EMULATOR` with a suitable Wine executable and use a temporary `WINEPREFIX` dedicated to tests. Add `-DNATIVE_MOTION_BUILD_SHADER_SMOKE_TEST=ON` for the optional ReShade/DirectX shader compiler test. The source tree's `tests/shader_compile/README.md` explains which matching compiler dependencies Proton needs. The mock `amd_fidelityfx_dx12.dll` stays in the build's test directory.

The source deliberately avoids ReShade virtual functions that return structs by value: MSVC and MinGW disagree on that ABI. Uniform handles are obtained through enumeration callbacks instead.

## Rendering and lifetime design

`fsr_probe.cpp` hooks FSR context creation/destruction and upscaler dispatch. For the exact process name `GTA5_Enhanced.exe`, device initialization loads the existing sibling `amd_fidelityfx_dx12.dll` with an absolute path and restricted dependency search, then installs hooks before the first context. It does not replace the provider file. Polling remains a fallback if early loading fails. `addon.cpp` inserts a GPU copy into the game's command list immediately before the original dispatch, then restores both resource states. The source resource is borrowed only within that callback.

Up to 32 lazily allocated capture textures form a bounded ring, with the display texture included in the shared 256 MiB allocation budget. Old-resolution capture textures are reclaimed only when free. ReShade command-list events associate copies with the actual submission queue. The initial implementation requires the runtime's graphics queue and presentation thread; other submissions are quarantined and capture stops. A slot becomes reusable only after a completion fence and replacement/destruction of the old command recording. A post-call detour of the observed native `ID3D12GraphicsCommandList::Reset` confirms successful replacement, including when GTA reuses that list for work other than FSR. A ReShade reset notification alone is not treated as successful native Reset. The detour pins its implementation module and forwards the original result unchanged. Other Reset implementations retain the conservative fallback: a new valid FSR recording on that list confirms replacement.

Before ReShade effects, the latest fresh capture is copied to a persistent display texture using ReShade's command-list API. Its SRV binding changes only on creation, resize or effect reload: ReShade's binding API waits for the graphics queue, so rebinding every frame would introduce an unnecessary stall. Texture resizing/runtime teardown can still wait for the queue. Normal capture has no CPU readback or per-frame GPU wait.

Submitted slots retire after real presentation, using a fence on the same queue. Unproven submissions are never recycled. If a device is torn down with unproven work, those bounded diagnostic allocations are retained until process exit rather than freed underneath the GPU.

Useful references: [AMD FSR scale conversion](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp#L870-L875), [ReShade add-on API](https://github.com/crosire/reshade/tree/v6.8.0/include), [ReShade texture-binding implementation](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime_api.cpp), [MinHook](https://github.com/TsudaKageyu/minhook).
