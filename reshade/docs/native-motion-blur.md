# Native per-object motion blur for GTA V Enhanced

**GTA V Native Motion Blur** uses the game's FSR 3.1 motion vectors to choose a blur direction and distance for each scene pixel. The add-on blurs the upscaled scene before GTA V Enhanced draws its HUD. A car can blur against a stationary background with a fixed camera when the game supplies separate velocities for the two surfaces. Camera motion can blur the scene during a pan. The add-on and vector diagnostic use the same native buffer. [Native motion-vector diagnostic](native-motion-overlay.md)

## Install and enable

With an existing compatible ReShade installation, extract `NativeMotionBlur-win64.zip` beside `GTA5_Enhanced.exe` and select `NativeMotionBlur.ini` in ReShade. The package supplies the add-on, both ReShade effects, and the preset. Install ReShade separately.

1. Close GTA V Enhanced. Use Story Mode with BattlEye disabled and the full add-on edition of ReShade. [Steam and Proton setup](native-motion-overlay.md#install-on-steam-and-proton)
2. With the Windows add-on built, install the blur preset from the source checkout’s `reshade/` directory (`cd reshade` from the repository root). Pass the existing graphics XML under the Proton prefix to back up the game's settings before editing them.

   ```bash
   python3 scripts/install_local_overlay.py \
     --game-dir '/absolute/path/to/Grand Theft Auto V Enhanced' \
     --reshade-dll '/absolute/path/to/ReShade64.dll' \
     --backup-root '/absolute/path/to/backups' \
     --settings-file '/absolute/path/to/Rockstar Games/GTAV Enhanced/settings.xml' \
     --effect blur
   ```

3. In the game's graphics settings, turn on FSR upscaling, turn frame generation off, and set **Motion Blur Strength** to zero to avoid applying both blur passes. The corresponding XML value is `<MotionBlurStrength value="0.000000" />` if editing the file while the game is closed.
4. Open ReShade with **Home** and verify **GTA V Native Motion Blur (before HUD)** is enabled. Use **F7** to toggle ReShade effects and **F8** to save a screenshot of the composed frame under `NativeMotionCaptures`. The installer also installs the vector diagnostic with its technique disabled in the blur preset. To inspect the vectors, disable the blur technique, enable **GTA V Native Motion Vectors**, and turn on **Show status marker** if you want the input indicator.

The Windows package also includes `NativeMotionBlur.ini`, which can be selected in an existing ReShade installation after extracting the package. The source installer uses the same preset and writes a journal to support preview and rollback. Pass `--restore-settings` to the restore script if you also want the original game graphics XML back. [Rollback instructions](native-motion-overlay.md#local-installer-and-rollback)

## Controls

| ReShade control | Default | Behavior |
| --- | --- | --- |
| Exposure mode | Shutter angle | Shutter angle uses a fraction of the current frame interval. Fixed exposure uses a duration in milliseconds, capped at one frame interval. |
| Shutter angle (degrees) | 180° | Exposes half the frame interval. A value of zero disables blur in this mode. |
| Exposure (ms) | 8.33 ms | Applies in Fixed exposure mode. It is approximately half a frame at 60 FPS. |
| Samples | 9 | Includes the center sample. More samples can smooth long streaks and require more texture reads. |
| Maximum radius (output pixels) | 12 px | Limits the distance from the center to each end of the blur path. A value of zero disables blur. |
| Display | Full image | Split comparison shows the original scene on the left and the blurred scene on the right. GTA composites the HUD over both halves. |
| Wheel blur path | Circular arcs | With the experimental wheel correction enabled, follows projected wheel rotation. Straight comparison uses the corrected displacement with the original linear gather. |

The supplied preset supplies these defaults. Controls remain readable by the add-on in Standard and Performance Modes. Disable Performance Mode while editing them in the UI. The add-on reads these settings and the effect's enabled state at presentation and applies them during the next eligible FSR dispatch. Changes made in ReShade can take one frame to appear.

## How the effect works

After a successful FSR upscale dispatch, the add-on records a D3D12 compute pass on the same command list. The pass reads the upscaled scene in linear color and the motion vectors for that dispatch. It writes the blur to a separate texture, copies the result into the game's FSR output, and restores the game's resource states. GTA then runs its display processing and composites the HUD. FSR accumulates temporal history in internal textures separate from the application's output, so the blur leaves that history intact. [AMD FSR integration guide](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/#placement-in-the-frame), [AMD upscaler resource bindings](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp#L810-L820)

The FSR motion field encodes displacement from the current frame to the previous frame. The shader point-samples the active region of the motion texture and uses the signed FSR scale and render dimensions to convert motion to output pixels. Exposure scales the displacement. The gather is centered on the current pixel, so the radius is half the exposure-scaled vector length before the radius cap. Blur starts above half a pixel and reaches full strength at one pixel.

Each color tap is downweighted or skipped if its native velocity disagrees with the center velocity. This reduces cross-silhouette bleeding. The gather uses one vector per sampled position and looks only at color and motion. It cannot recover newly revealed background or distinguish overlapping surfaces with similar velocities. Thin geometry, transparencies, and surfaces with missing or shared vectors can still produce artifacts. Coverage of skinned limbs and rotating parts depends on the vectors GTA V supplies.

Verified rigid wheel pixels can instead use the circular-arc path when `NativeWheelBlur.enable` is present at startup. This reconstructs the local wheel point from its private depth, rotates it about the axle, and projects each exposure sample into the scene. It validates wheel identity and final scene depth for each color contribution. The radius limit shortens exposure along the curve. Pixels without a valid wheel reconstruction retain the ordinary motion-vector gather. See [wheel implementation and limitations](enhanced-wheel-motion.md#circular-wheel-sampling).

The blur requires a tracked D3D12 command list submitted on one graphics queue and a single-sample, single-mip FSR scene texture. Supported scene output formats are `R16G16B16A16_FLOAT`, `R32G32B32A32_FLOAT`, `R11G11B10_FLOAT`, `R10G10B10A2_UNORM`, and `R8G8B8A8_UNORM` when the GPU supports sampling and typed UAV stores for the format. Frames pass through when input is unsupported or unavailable, when FSR requests a history reset or reports jittered vectors, or when its frame interval falls outside `(0, 250]` milliseconds. Individual invalid or outlier vectors are skipped. The pass and the vector diagnostic's copies run on the GPU.

## In-game verification

The completed benchmark verifies blur before HUD composition, using four scene-before/after pairs and corresponding final frames. [Pre-HUD test report](hud-blur-test-2026-11-27.md)

For automatic comparisons, temporarily add `GTA_NATIVE_BLUR_CAPTURE=1` before `%command%` in the Steam environment-variable portion. The add-on saves `NativePreHUD-*-SceneBefore.bin` and `NativePreHUD-*-SceneAfter.bin` under `NativeMotionCaptures`, with a JSON file giving the raw scene format, dimensions, and sample sequence. Use the matching `NativePreHUD-*-Final` screenshot to inspect HUD edges on a direct SDR RGBA8 or BGRA8 swap chain. These opt-in readbacks can cause brief stalls. Remove the flag for normal play.

Use the existing RX 7800 XT, Steam/Proton, FSR Quality, frame-generation-off setup as a starting point. The earlier benchmark verified the native motion input used by this effect. [Prior live motion-vector test](live-test-2026-11-27.md)

| Scene | What to compare |
| --- | --- |
| Fixed camera, static surface | Static detail should remain sharp. Animated portions can still carry vectors. |
| Fixed camera, passing vehicle | Streaks should follow the vehicle motion and stay largely within areas of coherent velocity. |
| Camera pan | Moving view detail can blur even where the world geometry is static. Look for sharp minimap and text edges against streaking detail. |
| FSR disabled | The blur should bypass until fresh supported FSR input resumes. |

Use **F8** to inspect the final composed frame at full resolution. For a manual comparison, select **Split comparison** and pan the camera with on-screen text visible.
