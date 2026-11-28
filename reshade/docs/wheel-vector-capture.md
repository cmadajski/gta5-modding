# Raw wheel-vector capture

The add-on can save four raw FSR motion-vector inputs together with the pre-HUD scene from each corresponding dispatch. This avoids the gain, saturation, blending and color interpretation of the diagnostic overlay. It works with the visualization and blur techniques disabled, provided GTA still invokes the hooked FSR upscaler. Capture itself does not synthesize motion. When the experimental wheel correction is enabled, the captured FSR input includes its upstream native-vector changes; metadata records whether it was enabled.

## Capture in Story Mode

1. Use FSR upscaling with frame generation disabled. Keep the camera still, close to the side of a wheel with identifiable spokes. A stationary burnout separates spin from vehicle translation; smoke and very fast repeated spokes can make the test inconclusive. Capture a stationary-wheel baseline too.
2. Press **Ctrl+F6** once while the spokes visibly move. Keep the camera and vehicle in view for another second. Four requests are recorded in distinct presentation intervals; the metadata separately records their actual submission intervals. The game may briefly hitch while diagnostic files are written.
3. Find `NativeWheel-<session>-01` through `-04` in the game's `NativeMotionCaptures` directory. Each item has a JSON description, `-Motion.bin`, `-SceneBefore.bin`, and `-SceneAfter.bin`. ReShade.log records requests, completed saves and failures. Wait for saves before another burst.

A host can request and archive the same burst without keyboard automation:

```bash
python3 scripts/capture_wheel_vectors.py \
  --game-dir '/absolute/path/to/Grand Theft Auto V Enhanced' \
  --output '/absolute/path/to/new-capture-directory'
```

The script creates `NativeMotionCaptures/CaptureWheel.request`. The add-on consumes it while presenting. A request expires after 15 seconds without enough eligible FSR samples. An unsupported queue or incomplete GPU retirement can prevent collection; the log distinguishes those cases. Capture allocation is capped at 512 MiB in total and may reject high-resolution bursts rather than overallocate.

## Analyze

```bash
python3 scripts/analyze_wheel_vectors.py '/absolute/path/to/capture-directory'
```

This creates scene previews, vector visualizations, numeric arrays and a report. For the wheel analysis, specify its ellipse center/radii and a nearby rigid chassis rectangle in **output-image pixels**:

```bash
python3 scripts/analyze_wheel_vectors.py '/absolute/path/to/capture-directory' \
  --wheel 1280 800 90 90 --reference 1170 620 150 40
```

Those numbers are examples, not a detected wheel. Inspect the scene preview to choose them. The report subtracts the reference median, summarizes tangential/radial residuals and fits an elliptical circulation field. It does not interpret a low residual as proof of missing spin: visible spoke movement, unambiguous frame spacing, correct region selection and adequate resolution are required. A field present in one material does not establish coverage of every rim, tire, LOD or brake component.

The raw file is tightly packed, row-major, top-left origin, in the original reported DXGI format. It excludes inactive allocation padding and preserves encoded channel bits. The conversion to output-pixel displacement is `raw * FSR_scale * output_size / active_motion_size`. The original direction is current-to-previous. Metadata includes render/output extents, signed scale, timing, jitter/reset flags, dispatch identity, submission count and recording replacement. Replayed recordings and duplicate submission intervals are unsuitable as proof of consecutive wheel motion. Before/after scenes are linear game output, so the previews apply an illustrative tone map; raw values remain available.

## Verify circular wheel blur

With circular wheel blur active, the first item in a burst can additionally export `-WheelMask.bin`, `-WheelDepth.bin`, `-SceneDepth.bin`, and `-WheelParameters.bin`. These optional snapshots are taken before the private mask is cleared. Their allocation can fail independently of the base capture; check the JSON capture flags before using them. They preserve the native padded allocation, unlike the cropped motion image.

The mask is RGBA32F with `(slot + 1) / 65536` in alpha. Private depth is float32. Final depth uses the reported copied-plane format and row width; the verified Proton `R32G8X24_TYPELESS` input exports float32 depth plane zero. Parameters use the 512-byte [GPU layout](../include/wheel_arc_math.hpp), including current-frame ownership and the matrices actually prepared for submission.

```bash
python3 scripts/analyze_wheel_arcs.py '/absolute/path/to/capture-directory' \
  --out '/absolute/path/to/arc-analysis' --shutter 180 --samples 9 --radius 12
```

Pass the exposure, sample count, radius and split-view option that were active during capture. The analyzer validates frame stamps, final depth and native motion, then compares the GPU result against curved and straight references from the same input image. It also compares a chord with the same ownership checks and weights to distinguish curvature from changes in sample rejection. See the [in-game arc test](wheel-arc-test-2026-11-27.md).

## Installed-build evidence

[Enhanced shader inspection](enhanced-wheel-motion.md) found native current/previous wheel transforms and a deferred wheel motion output. [The live test](wheel-vector-test-2026-11-27.md) found no coherent rotational field on two visibly spinning wheels in the captured FSR input. The follow-up native draw trace checks the matrices and deferred target to locate the correction.

For the optional draw trace, create `NativeWheelTrace.enable` beside the executable before starting GTA. The same Ctrl+F6 burst then logs recognized wheel shader hashes, target 4, descriptor identity, and read-only current/previous matrices when their buffer is CPU-visible. Remove the marker and restart to disable this extra tracking. Trace lines are bounded and prefixed `[NativeWheelTrace]`; CPU snapshots reflect command-recording time and do not by themselves prove executed GPU contents.

With wheel correction enabled, bursts also produce lightweight draw, depth-copy and mask diagnostics without the heavy trace marker. Copy observations establish recording-time resource relationships; the arc shader separately checks final depth and native motion at each sampled texel.
