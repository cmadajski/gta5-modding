# Pre-HUD native motion blur — live validation, 2026-11-27

Local `local-testing/...` paths in this historical report are relative to the repository root. Those ignored captures and backup journals remain there after source organization into `reshade/`.

The blur now runs immediately after GTA V Enhanced's FSR upscale dispatch, before the game composites the HUD. A complete five-pass Story Mode benchmark on the RX 7800 XT confirmed scene blur with sharp benchmark text. Four captures contain the native scene before blur, the scene after blur, and the corresponding composed frame. Both scene captures lack the FPS counter and exit prompt; the final frame contains those overlays.

## What changed

The FidelityFX hook publishes a second callback only after a successful original dispatch. A monotonic dispatch identity connects it to the exact motion texture copied before that dispatch. The native compute pass copies FSR's active output into an owned scene texture, gathers color along the native motion vectors into a separate result, and copies that result back into the active FSR output. It restores the resource's declared D3D12 state. The scene remains in its linear HDR domain throughout the gather.

`NativeMotionBlur.fx` now provides controls and an unchanged presentation pass. Its technique checkbox and ReShade's global effects toggle govern the native pass, with settings cached at presentation for the next eligible dispatch. There is no filtering fallback on the composited frame. The packaged preset retains the 180° shutter, nine samples and 12-output-pixel maximum radius.

FSR keeps its temporal history in separate internal resources. Its DX12 backend restores externally registered resources to their declared initial states before returning. These contracts are the basis for the insertion point; the live scene captures establish that the tested game's HUD actually comes later. [AMD resource bindings](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp#L810-L820), [AMD state restoration](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/src/backends/dx12/ffx_dx12.cpp#L1822-L1840)

## Tested configuration

| Component | Observed configuration |
| --- | --- |
| Game | GTA V Enhanced, Steam app 3240220, build 25971420 |
| Host | Linux, Flatpak Steam, RX 7800 XT / RADV NAVI32 |
| Compatibility | Proton Experimental `experimental-11.0-20261123b-x86_64` |
| ReShade | 6.8.0 full add-on edition, D3D12 |
| Native motion | RG16F, 1706 × 960 active, FSR scale `(1, 1)` |
| FSR scene | RGBA16F, 2560 × 1440, UAV state (`0x8`), resource flags `0x5` |
| Settings | FSR Quality, frame generation off, game Motion Blur Strength zero |
| Mod | Full-image display, 180° shutter, nine samples, 12 px radius |

FSR recording occurs on game worker threads. The add-on serializes its CPU state, uses the original native command list for its native descriptor heaps, and restricts shared scratch use to command lists previously observed on the presentation graphics queue. A queue change disables subsequent processing. This is a guarded implementation for the observed queue arrangement, not general asynchronous-compute support.

## Live capture evidence

The completed run is in `local-testing/prehud-run-01`. The benchmark wrote `Benchmark-26-11-27-19-09-11.txt` with passes 0–4; ReShade logged normal exit at 14:09:15 local time. Each capture metadata file records one submission and `final_same_frame: true`.

| Capture | Scene | Motion sample sequence | Scene RGB pixels changed |
| --- | --- | ---: | ---: |
| 01 | Night street | 625 | 1,030,161 / 3,686,400 (27.94%) |
| 02 | Pier | 3,053 | 292,297 / 3,686,400 (7.93%) |
| 03 | Banking jet | 7,109 | 3,471,461 / 3,686,400 (94.17%) |
| 04 | SUV chase | 13,213 | 2,303,219 / 3,686,400 (62.48%) |

These are exact decoded **linear HDR RGB** comparisons, not perceptual difference scores. Small FP16 changes can count even when invisible in the final image. Alpha was bitwise identical in all four pairs, and neither input nor output contained nonfinite components. Flight and driving captures visibly show background streaking with sharper tracked vehicles; the FPS counter and exit prompt remain sharp above those scenes.

The last periodic snapshot recorded 16,329 accepted FSR dispatches, 16,242 motion copies and **15,909 native blur command recordings**, with zero reported motion-buffer drops, rejections, quarantined submissions or allocation failures. Native renderer allocation failures and its internal skip counter were also zero. These are pre-exit snapshots. The record count is 97.43% of accepted dispatches; this is not a claim that every dispatch receives blur. Startup, queue-learning, history/timing guards and lock contention can bypass it, and guard skips outside the renderer are not included in its internal skip counter.

`NativePreHUD-03-HUD-crops.png` compares the current scene-before, scene-after and final FPS region with the earlier post-HUD effect's before/after FPS region. The two versions show different benchmark frames and are labeled accordingly. The new raw pair itself is a same-frame comparison. Raw previews use an illustrative Reinhard/sRGB mapping, so their brightness does not reproduce GTA's tone mapping; use `*-Final.png` for the displayed result.

## Regression checks

The portable CPU suite passed 1/1. Four Windows hook/compiler tests passed under Proton: callback success/failure and identity rules, vector FX compilation, blur-control FX compilation in both ReShade modes, and native compute compilation/reflection. The native shader exposes six bindings, 64 bytes of root constants and 8 × 8 threads.

An additional D3D12 GPU regression passed using Proton's native DXVK/vkd3d-proton libraries. In an HDR scene with padded dimensions and mixed motion, 70 moving pixels changed; all split-reference pixels, stationary pixels, alpha values and output padding stayed exact. A later GPU pass on the same command list saw the updated scene. The fixture also verifies the history-reset guard and final in-place destination. Point sampling avoids a texture-load compilation path that returned incorrect center pixels in the initial GPU test. [GPU test and reproduction](../tests/native_blur_gpu/README.md)

The tested vkd3d-proton build returned stale pixels for an inline texture-to-buffer readback of the reused output. The final destination check therefore reads it on a subsequent submission; same-list GPU-consumer checks passed independently. Live before/after diagnostics read distinct scene/result resources and wait for the recorded submission's fence and command-list replacement before collecting them.

## Installed state and scope

Normal launch options were restored to `WINEDLLOVERRIDES="dxgi=n,b" %command% -nobattleye`. Automatic captures and benchmark flags are removed; pause-on-focus-loss is restored. The full-image preset remains enabled, with GTA's original motion blur at zero. Home opens ReShade; F7 toggles effects.

The backup of the previous working post-HUD mod is `local-testing/prehud-backups/20261127T185253Z/install-journal.json`. The original test archive remains at repository-root `build-windows/NativeMotionBlur-win64.zip`; new builds use `reshade/build-windows/`. [Controls and rollback](native-motion-blur.md)

This run verifies benchmark text on the tested SDR output. Minimap, subtitles, menus, display HDR, frame generation and other upscalers were not separately exercised. Per-object coverage still depends on GTA's native vectors; the gather does not reconstruct occluded surfaces. The benchmark includes diagnostic readbacks and has no matched disabled-effect baseline, so it does not isolate performance overhead.

Tested add-on SHA-256: `0f42c575b5d9b7fc8d0c82bb75f0c2a475aa1db6b519e7b3fea39a04dc6224bc`.
