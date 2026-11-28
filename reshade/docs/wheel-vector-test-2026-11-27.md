# GTA V Enhanced wheel-vector test, 2026-11-27

The spokes changed position against the brake caliper in two consecutive-frame bursts. The raw FSR inputs contained no coherent spoke-scale rotation on the sampled rear and front wheels. In the clearer front-wheel burst, an ellipse model of the visible spoke shifts predicted 20.2–26.3 output pixels of median displacement. The actual native input magnitude was 0.190–0.291 pixels at the median across the same three frame pairs.

## Capture evidence

The tested configuration was GTA V Enhanced Steam build `25971420`, D3D12 through Proton, with ReShade 6.8 and FSR Quality. The FSR input was original RG16F at 1706 × 960 with dispatch scale `(1, 1)`. The matching scene-before readbacks were RGBA16F at 2560 × 1440 after upscale and before the add-on blur or game HUD. The blur pass was enabled in all captures. Measurements use the scene before that pass and the original motion input. [Capture procedure](wheel-vector-capture.md)

| Subject | Capture filename prefix, items 01–04 | Sample sequence | FSR dispatch ID | Recorded and submitted presentation intervals |
| --- | --- | --- | --- | --- |
| Rear wheel, moving across the frame | `NativeWheel-134402885336504812-2400-12722` | 9000–9003 | 9062–9065 | 12723–12726 |
| Front wheel, near side view with hub nearly fixed on screen | `NativeWheel-134402886248007282-2400-22718` | 13881–13884 | 19059–19062 | 22719–22722 |
| Stationary-wheel baseline | `NativeWheel-134402885839916176-2400-18516` | 10929–10932 | 14869, 14874, 14877, 14878 | Recorded 18528, 18533, 18536, 18537. Submitted 18529, 18534, 18537, 18538. |

Every recorded item reported `source_same_dispatch=true`, `submissions=1`, and `recording_replaced=true`. Reset, jitter cancellation and display-resolution vector flags were false. In the spinning bursts, sequence, dispatch and submission interval each advanced by one within the same FSR context. The analyzer decoded the expected payload sizes, found no metadata issues, and accepted all 1,637,760 active motion texels in each spinning frame. The baseline dispatches had gaps. Its spoke positions stayed visibly fixed, and its active motion fields were zero across all four captures. [Stationary-wheel scene strip](../../local-testing/wheel-live-check-01/wheel-frame-strip.png), [baseline vector report](../../local-testing/wheel-live-check-01/rear-wheel/wheel-vector-analysis.json)

## Regions and measurements

All coordinates use 2560 × 1440 output pixels with the origin at the upper left. Wheel ellipses use `(center x, center y, radius x, radius y)`. Reference rectangles use `(x, y, width, height)`.

| Subject | Wheel ellipses in capture order | Chassis reference |
| --- | --- | --- |
| Rear | `(1453, 1159, 101, 119)`, `(1441, 1159, 101, 119)`, `(1426, 1159, 101, 119)`, `(1411, 1159, 101, 119)` | `(1090, 980, 180, 85)` |
| Front | `(736, 1125, 112, 104)` for 01–02, `(737, 1128, 112, 104)` for 03–04 | `(1000, 1014, 145, 94)` |
| Stationary baseline | `(855, 1130, 108, 112)` | `(1100, 990, 380, 80)` |

The two spinning bursts use texels mapped to the wheel annulus between 0.28 and 0.88 of normalized radius. The analyzer converts raw RG to output displacement componentwise as `raw × FSR scale × output extent / active motion extent`, subtracts the median chassis vector, and fits an elliptical tangent field with a free constant offset. The free offset separates wheel-local translation from circulation. [Analyzer](../scripts/analyze_wheel_vectors.py)

### Rear wheel

The spokes visibly changed orientation against the red caliper. The hub shifted by 12–15 pixels horizontally between observed frames. The median residual magnitude after subtracting chassis motion was 5.26–6.53 pixels across the four captures. The residual was predominantly a constant offset at the wheel. After subtracting the wheel annulus's own median vector, the 95th-percentile residual was 1.98–2.59 pixels. The circulation fit explained 5.2–6.2% of the squared variation left by translation alone. The oblique view and changes in appearance made the image-based phase estimate unreliable for this burst. [Rear wheel scene strip](../../local-testing/wheel-independent/candidate-wheel-01.png), [rear wheel vector report](../../local-testing/wheel-independent/run-01/wheel-vector-analysis.json)

### Front wheel

The spokes visibly changed orientation against the red caliper while the hub stayed nearly fixed on screen. The scene background shifted with the camera. Normalized cross-correlation of polar scene luminance, excluding the caliper sector, yielded these apparent spoke phase changes. Positive phase would be clockwise in the image. The repeating spokes create angular ambiguity. The ellipse model converts the apparent shift and hub displacement to a projected current-to-previous vector for each annulus sample.

| Sample pair | Apparent spoke phase | Image correlation | Modeled median displacement | Native median magnitude |
| --- | ---: | ---: | ---: | ---: |
| 13881 → 13882 | −17.5° | 0.65 | 21.4 px | 0.190 px |
| 13882 → 13883 | −16.5° | 0.70 | 20.2 px | 0.256 px |
| 13883 → 13884 | −21.5° | 0.74 | 26.3 px | 0.291 px |

The correlation scores measure agreement between shifted scene-luminance profiles. The native medians use the un-subtracted current-frame vector field. Across all four captures, the chassis-subtracted median was 0.047–0.162 pixels and the 95th percentile was 0.081–0.171 pixels. Circulation explained 0.20–0.45% of the squared variation left by translation alone. In each capture, 0.94–1.32% of annulus samples exceeded 1 pixel after chassis subtraction. These localized samples pointed almost directly left, largely along visible brake-disc or spoke-gap edges. Their source remains unidentified. Moving the ellipse centers by ±5 pixels and scaling the radii by 0.85–1.05 kept the 95th-percentile residual at or below 0.172 pixels and the fit below 0.64% of centered variation. [Front wheel scene strip](../../local-testing/wheel-independent/candidate-wheel-02.png), [image comparison data](../../local-testing/wheel-independent/front-wheel-image-motion-comparison.json), [front vector report](../../local-testing/wheel-independent/run-02/wheel-vector-analysis.json), [large residual locations](../../local-testing/wheel-independent/residual-outliers-02.png)

## Scope and next trace

The result applies to the tested FSR input, vehicle, views and visible wheel regions. The displacement estimates depend on an approximate projected ellipse and repeated spoke features. The measurements leave the true per-frame rotation and the source of the sparse vectors unresolved. The scene strips are illustrative previews and the linked local captures are ignored by Git.

The add-on has no wheel-specific blur fix. The next trace is to identify the native draw and material that produce the visible spokes, inspect their bound current and previous wheel matrices, and follow their deferred motion target into the FSR input. [Enhanced shader inspection and trace plan](enhanced-wheel-motion.md)
