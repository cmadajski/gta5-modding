# Native wheel correction: 2026-11-27

The experimental rigid-wheel correction completed GTA V Enhanced's Story Mode benchmark on the RX 7800 XT through Steam/Proton. The captured motion field drives visible tire-tread blur before the HUD, while the adjacent vehicle body stays sharp. A separate passing-car capture shows coherent wheel circulation. This verifies the tested surfaces and frames; a true curved blur remains separate rendering work.

## Build and setup

- Steam app `3240220`, game build `25971420`, official full-add-on ReShade 6.8.0.
- Proton Experimental `experimental-11.0-20261123b-x86_64`, RADV NAVI32.
- FSR 3.1 input: RG16F, 1706 × 960; pre-HUD output: RGBA16F, 2560 × 1440. Frame generation disabled.
- Blur: 180° shutter, nine samples, 12-output-pixel maximum radius.
- Add-on SHA-256: `a4fd03c8c12212609d64981d080cb50dba0437acb11e6604fadff145472a0f0a`.
- `NativeWheelBlur.enable` present; heavy `NativeWheelTrace.enable` absent.

## Verification

Three host CTest targets and eight Windows/Proton targets passed. These include captured-matrix regressions, FX/compute compilation, raw-vector readback, native descriptor copying and Device15 writers, draw binding restoration, and native wheel submission. The authored runtime GPU fixture verifies CPU writes after recording and before submission, stock replay, a deliberately stalled queue, fence retirement, command-list replacement, and startup teardown on a different thread. The synthetic fixture supplies its own shader identity; the real game's shader matches are established by the live run.

The first trace crash exposed a MinGW/MSVC aggregate-return ABI mismatch. An isolated fixture reproduced that failure; the replacement observes render-target identity through callbacks instead. Two corrected trace benchmarks completed before the wheel correction was enabled. Subsequent checks also identified newer Device15 descriptor writers and startup swapchain recreation; both have regression coverage.

The enabled run, `local-testing/wheel-correction-benchmark-03`, exited normally after 255.26 seconds, including launch/loading. Its last periodic status recorded:

| Observation | Count |
| --- | ---: |
| Matched native pipeline attempts | 206,780 |
| Queue-admitted and source-resolved draws | 206,672 |
| Draws using private constants | 197,140 |
| Submitted previous-rotation corrections | 59,297 |
| Replay, source fallback, wait failure, dropped or unsupported draw counters | 0 |
| Runtime blocked | 0 |

The highest sampled private upload use was 277,504 bytes within a fixed 32 MiB pool. These are draw counts, including repeated materials and frames, not counts of distinct wheels. A private-constant draw can retain stock values when no correction passes the fit.

All 24 raw capture sets have matching byte sizes, one observed submission each, completed recording replacement, and `wheel_correction_enabled=true`. Each of the six bursts has four adjacent FSR dispatches and actual presentation intervals. Dispatch ranges are 1018–1021, 5092–5095, 9060–9063, 9654–9657, 9963–9966 and 10534–10537.

## Visible tire result

In dispatches 9654–9657, the Mammoth's left rear tread has median native motion magnitudes of approximately 37–41 output pixels, and 37–42 pixels after subtracting the nearby chassis reference. The blur changes 3,334–3,352 of 3,396 wheel-region pixels in the matching before/after images. Its mean vertical luminance gradient drops about 75–78%. The mean linear RGB change in the adjacent chassis reference stays below 0.00000684.

The later tread sample, dispatches 10534–10537, has median motion of 36–50 pixels and visible blur throughout its 3,108-pixel wheel region. Earlier disabled runs had much smaller, mostly translational tread motion. The benchmark runs follow different positions and frame timing, so this is not a pixel-identical enabled/disabled comparison. The same-frame before/after pairs establish that the pre-HUD filter blurs the newly available tire motion. The edge-on tread view does not establish a full face-on circular vector field.

A side-on passing car in dispatches 9963–9965 shows coherent circulation after subtracting chassis motion. The front-wheel ellipse fit explains 91.6–91.9% of residual variation, with the same tangential sign in all eight angular sectors and median residual motion of 3.42–3.69 pixels. The rear-wheel fit explains approximately 81.8% in two frames. Dispatch 9966 returns to a nearly uniform wheel field, so correction coverage is not continuous for every observed wheel or frame. The moving body also blurs in these views; the Mammoth captures supply the clearer separate evidence of isolated tread blur.

Local evidence, excluded from Git because it includes game captures:

- [Enabled capture manifest](../../local-testing/wheel-correction-benchmark-03/result.json) and [ReShade log](../../local-testing/wheel-correction-benchmark-03/ReShade.log).
- [Compact before/after/vector figure](../../local-testing/wheel-correction-analysis/wheel-correction-9654.png) and [side-on circulation](../../local-testing/wheel-correction-analysis/cross-traffic-circulation-9963.png).
- [Four-frame tread/vector comparison](../../local-testing/wheel-correction-analysis/enabled-04-left.png) and [numeric measurements](../../local-testing/wheel-correction-analysis/enabled-04-left.json).
- [Later tread comparison](../../local-testing/wheel-correction-analysis/enabled-06-left.png).

## Installed state and limits

The tested add-on and startup marker are installed for normal Steam launches. The enable journal is `local-testing/wheel-correction-enabled-backups/20261127T224856Z/install-journal.json`. Temporary benchmark command-line flags were removed and graphics settings were restored byte-for-byte. The ReShade preset, game executable and archives were not edited by this update. Remove `NativeWheelBlur.enable` and restart to disable the wheel correction; the existing general motion blur remains controlled by ReShade.

Coverage is restricted to the identified rigid tire shader variants, verified root layout, CPU-visible constants and tracked direct queue. Unsupported draws retain the stock path. The runtime assumes the game finishes CPU writes to the source constants before native submission. Corrected two-frame vectors still describe straight displacement; they do not recover full revolutions or produce an exact circular shutter path. See [implementation and limits](enhanced-wheel-motion.md).
