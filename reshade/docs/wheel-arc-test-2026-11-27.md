# GTA V Enhanced curved wheel blur test, 2026-11-27

The final Story Mode benchmark verified curved wheel blur in GTA V Enhanced. Across 4,807 output pixels where curved and straight paths with identical ownership checks gave distinguishable predictions, the curved reference matched the GPU result more closely in 98.65% of cases. The authored D3D12 fixture also verified quarter-circle sampling directly.

## Configuration

- GTA V Enhanced Steam app `3240220`, build `25971420`, official full-add-on ReShade 6.8.0, RX 7800 XT and Proton Experimental.
- FSR 3.1 Quality, frame generation disabled. The native RG16F vectors are 1706 × 960 and the pre-HUD RGBA16F scene is 2560 × 1440.
- Full-screen blur, 180° shutter, nine samples and a 12-output-pixel radius cap. The wheel correction marker was present and the full shader trace was disabled.
- Each run requests six bursts at 20, 90, 138, 145, 150 and 160 seconds after the first observed FSR sample. Each burst captures four frame pairs. The first frame can also capture the private wheel mask, depths and parameter pool. [Capture procedure](wheel-vector-capture.md), [arc implementation](enhanced-wheel-motion.md#circular-wheel-sampling)

## Isolated verification

The four host CTest targets and nine Windows/Proton targets passed. The CPU arc tests check inverse projection, viewport aspect, the perspective path across thick tread and the radius cap. The runtime fixture exercises native submission, corrected history and stock fallback. The shader build also checks the compiled compute bindings. [Host tests](../../local-testing/wheel-arc-benchmark-04/host-tests.log), [Windows/Proton tests](../../local-testing/wheel-arc-benchmark-04/windows-tests.log)

The authored D3D12 fixture passed through Proton with `R32_TYPELESS` and `R32G8X24_TYPELESS` final depth. It uses a 96 × 72 output, cropped 64 × 48 motion input, padded 66 × 50 mask and +0.4 render pixels of jitter. The depth-stencil case alternates between two final depths distinct from the stable wheel depth attachment. Two scene colors at the quarter-circle endpoints each contribute 0.25 red or green to the selected center sample, within float tolerance. The fixture verifies shared ownership across two draws of one wheel, rejection of occluded or foreign-wheel taps, curved sampling under the 12-pixel cap and unchanged alpha. A tap is rejected when its native endpoint disagrees even at matching depth. A mismatched center endpoint or stale frame stamp produces identical whole-frame straight output. For each depth format the final depth readback uses `R32_TYPELESS` with four bytes per texel and contains exact depths of 0.5 and 0.75 at selected texels. [GPU fixture](../tests/wheel_arc_gpu/gpu_test.cpp), [CPU arc tests](../tests/wheel_arc_math_tests.cpp)

## Depth diagnosis in the first two live runs

| Run | Start, Eastern time | Captured dispatch span | Scheduled burst frames | Submitted correction draws | Fitted stationary draws | Arc frames |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| 01 | 20:19:21 | 1001–10444 | 24 | 233 | 288 | 0 |
| 02 | 20:26:30 | 1021–10569 | 24 | 249 | 467 | 0 |

Both benchmarks exited normally. The table sums draw decisions across captured frames and includes repeated materials and vehicles. The gaps in dispatch IDs separate bursts. Every run 01 frame established shared scene and motion provenance and one observed submission with a replaced recording. No frame exported verified wheel auxiliary data in either run.

Run 01 archive fields marked depth invalid. In run 02 the diagnostic exposed the native allocation as `R32G8X24_TYPELESS`, 1706 × 960, one mip, one sample, reversed Z, and the combined pixel and compute shader read state. The arc renderer now binds an `R32_FLOAT_X8X24_TYPELESS` view of plane zero and restores its state after use. Readback captures the same depth plane at its native footprint. [Run 01 manifest](../../local-testing/wheel-arc-benchmark-01/result.json), [run 02 log](../../local-testing/wheel-arc-benchmark-02/ReShade.log)

An independent NumPy reference ran the straight shader's sample positions, motion agreement and color weighting against each run 01 `SceneBefore` and its captured motion. Across 275,424 finite grid samples, 171,075 changed by at least 0.002 mean decoded RGB relative to the GPU result. On that subset, the straight reference's mean absolute RGB error was 0.004824 and the unblurred scene's error was 0.669756. This establishes straight sampling as the observed baseline for this run. [Reference script](../scripts/analyze_wheel_arcs.py), [run 01 report](../../local-testing/wheel-arc-analysis/wheel-arc-oracle.json)

## Alternating depth allocations in run 03

Run 03 started at 20:33:47 Eastern time. Its first 16 FSR dispatches reported valid depth and alternated between two distinct native depth allocations. The initial integration discarded each private mask's initialized state when the depth allocation switched. Initialization now persists separately for each verified surface, and invalidation clears all remembered surfaces when history is lost.

The run archived 28 frames, including an additional requested diagnostic burst. All 28 validated the FSR depth description and retained the straight blur. Across 644 logged wheel draws that targeted the exact FSR motion allocation, all bound a single native scene DSV distinct from both FSR depth allocations. This shows why matching the wheel draw's DSV pointer directly to the dispatch depth rejected it. The private wheel depth, final FSR depth and measured native motion endpoint are now compared at the same jittered raster texel. Each color contribution requires all three to agree. [Run 03 log](../../local-testing/wheel-arc-benchmark-03/ReShade.log), [run 03 manifest](../../local-testing/wheel-arc-benchmark-03/result.json)

## Verified live result in run 04

Run 04 started at 20:45:04 Eastern time and exited normally after 253.97 seconds including launch and loading. All 24 captures shared their FSR dispatch with the blur pass, observed one submission and replaced the recording before export. Submission intervals and dispatch IDs advanced by one within each of the six bursts. The first item in each burst includes the wheel mask, private and final depth, and submitted parameter pool. The stable native DSV identity is distinct from both alternating FSR depth resources. [Manifest and SHA-256](../../local-testing/wheel-arc-benchmark-04/result.json), [ReShade log](../../local-testing/wheel-arc-benchmark-04/ReShade.log)

| Dispatch | Presentation interval | Valid submitted arc draws | Masked render texels | Final depth agrees | Frame, wheel, depth and vectors agree | Eligible output pixels |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 980 | 4816 | 0 | 0 | 0 | 0 | 0 |
| 5070 | 10880 | 0 | 1,354 | 0 | 0 | 0 |
| 8966 | 14776 | 23 | 30,090 | 8,483 | 7,686 | 17,243 |
| 9482 | 15292 | 28 | 52,589 | 16,630 | 10,652 | 24,025 |
| 9793 | 15603 | 36 | 45,330 | 8,932 | 8,932 | 20,062 |
| 10354 | 16164 | 12 | 29,389 | 5,358 | 5,251 | 11,792 |

Every render texel that passed final depth and current wheel identity also passed the native vector endpoint check. The 95th-percentile endpoint residual was 0.014–0.025 render pixels across the four populated arc snapshots. A shader tap can still be clipped by the output bounds, radius, or neighboring visibility. [Per-dispatch and per-draw measurements](../../local-testing/wheel-arc-analysis/run-04/wheel-arc-oracle.json)

The reference sampled all three models from the same `SceneBefore`. The legacy model uses the original straight motion-vector gather. The matched-ownership chord uses the curved path's endpoints, radius, tap weights and visibility checks, and interpolates linearly from the center to each endpoint. The matched-ownership comparison isolates the effect of the curved path. The table includes pixels where the two predictions differ by at least 0.002 mean absolute decoded RGB. Errors compare each prediction with the GPU scene.

| Samples and comparison | Distinguishing output pixels | Arc error | Straight error | GPU closer to arc |
| --- | ---: | ---: | ---: | ---: |
| All verified snapshots, legacy straight path | 28,401 | 0.000157 | 0.084730 | 99.99% |
| All verified snapshots, matched-ownership chord | 4,807 | 0.000277 | 0.007272 | 98.65% |
| Dispatch 9482, Mammoth tread wheel 3, legacy straight path | 3,993 | 0.0000414 | 0.020930 | 100% |
| Same tread, matched-ownership chord | 867 | 0.0000686 | 0.004353 | 100% |

The pictured tread has an estimated signed roll magnitude of 36.45° over the frame interval. The fitted roll describes the rotation between observed orientations. The GPU result follows the curved model on the same tread pixels. [Tread comparison](../../local-testing/wheel-arc-analysis/run-04/NativeWheel-134403041206657477-2324-15291-01-wheel-3-arc.png), [second wheel comparison](../../local-testing/wheel-arc-analysis/run-04/NativeWheel-134403041206657477-2324-15291-01-wheel-4-arc.png)

Across all 24 sampled frames, 812 draws reached submission. There were 356 validated corrections with arc parameters, 271 stationary fits, 177 rejected fits and eight missing prior meshes. There were zero record-admission rejections or missing-history frames in this sample. The last periodic benchmark log recorded 194,042 mask draws, 61,257 arc-valid draws of 62,645 corrected draws, and 9,148 arc compute recordings of 10,842 blur recordings. These are draw and dispatch counts, which include repeated materials and frames. The sampled private upload peak was 749,568 bytes. All recorded replay retention, fence failure and invalid-projection counts were zero.

The reported average FPS across benchmark passes 0–4 was 68.80, 59.72, 74.43, 109.10 and 78.28 in run 03, and 67.80, 57.70, 74.06, 108.89 and 75.88 in run 04. The runs include differing capture work and live scene timing. These comparisons leave the GPU cost of the arc pass in isolation unresolved. [Run 03 benchmark output](../../local-testing/wheel-arc-benchmark-03/rockstar-benchmark.txt), [run 04 benchmark output](../../local-testing/wheel-arc-benchmark-04/rockstar-benchmark.txt)

## Coverage and interpretation

Earlier native-vector testing established visible tire blur and wheel circulation. In its fourth passing-car frame, the centered spokes change very little against the preceding frame. That flat residual does not establish a missed correction. The new runtime fixture reproduces a separate coverage failure caused by preliminary draw-recording constants and verifies that deferring the fit until native submission recovers a correct final pose. Its inverse case still retains stock motion when the finalized origin fails validation. [Earlier wheel correction test](wheel-correction-test-2026-11-27.md), [passing-car frame analysis](../../local-testing/wheel-correction-analysis/traffic-9963-9966-phase-and-field.json), [runtime fixture](../tests/wheel_runtime_gpu/gpu_test.cpp)

Verified coverage applies to the traced rigid tire shader variants, captured camera angles and supported direct-queue submissions. The signed fit uses the principal rotation in ±180° between two poses. A faster physical spin can alias to a smaller or opposite fitted angle, and stock fallback still applies when geometry or history is ambiguous. The model approximates intermediate camera and chassis motion. The single-frame gather cannot recover hidden spokes, full turns between frames or a complete exposure at disocclusions. The before/after captures and reference outputs are local artifacts excluded from Git.

The verified add-on and ReShade effect remain installed, with the wheel correction marker present. The game is closed. Graphics settings were restored byte-for-byte and the temporary benchmark command line and tracing were removed. [Final installation verification](../../local-testing/wheel-arc-benchmark-04/final-install-verification.json)

## Accepted baseline

On 2026-11-27, the owner accepted this version as the working baseline after reporting a significant visual improvement in side-profile wheel blur during normal play. That is a subjective playtest assessment. The instrumented benchmark evidence above remains primarily tread-focused. An [oblique alloy-rim capture](../../local-testing/wheel-arc-analysis/run-04/NativeWheel-134403041206657477-2324-15291-01-wheel-5-arc.png) also shows arc sampling on the rim face, but its fitted roll is only about 2.4 degrees per frame; it does not establish rapid, clean side-profile spoke blur across all rim materials.

Thin motorcycle and dirt-bike spokes remain a possible refinement. The current arc filter requires the center and every contributing sample to belong to the same currently visible wheel. It therefore does not sweep spoke color into gaps currently showing the background or a separately drawn brake assembly. Improving that changing coverage over the exposure is deferred; the accepted renderer and default controls are retained.
