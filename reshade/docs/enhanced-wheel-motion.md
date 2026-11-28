# Wheel motion in GTA V Enhanced

On Steam app `3240220`, build `25971420`, the deferred tire shaders compute per-pixel motion from current and previous wheel transforms. In the Story Mode benchmark, rigid tire draws wrote to the same render target that GTA passed to FSR 3.1. The CPU-side previous transforms recorded for the rapidly spinning rear wheels of the chase vehicle reused the current wheel roll. Rim and brake coverage can vary by material and remains unverified outside the traced draws.

## Enable the experimental correction

Close GTA V Enhanced. Add `--wheel-blur` to the local installer command, or create an empty `NativeWheelBlur.enable` file beside `GTA5_Enhanced.exe`. Launch Story Mode with the full add-on build of ReShade, FSR 3.1 and frame generation disabled. Enable `NativeMotionBlur.fx` for the pre-HUD blur effect. The correction reads the marker at startup. Remove it and restart GTA to disable the correction. Installation writes the add-on, marker and requested ReShade files. The game executable and archives are read-only inputs to the research. [Local installer and rollback](native-motion-overlay.md#local-installer-and-rollback), [Blur installation and controls](native-motion-blur.md)

The optional `NativeWheelTrace.enable` marker enables heavier matrix and descriptor logging. Leave it absent for normal play; Ctrl+F6 raw-vector capture works without it.

## Shader identification

This build stores `sga_win32_60_final.awc` and `sga_win32_60_final_init.awc` under `common/shaders/fxdb/` inside `update/update2.rpf`. The main `SGD2` library contains the `vehicle_tire` and `vehicle_tire_emissive` effects. The CRCs below are IEEE CRC32 of the entire `.cso` container, as used by ReShade's shader replacement example. The matching vertex and pixel variants are identified by their effect membership, function suffix, and compatible signatures. [ReShade shader replacement example](https://github.com/crosire/reshade/blob/main/examples/06-shader_replace/shader_replace_addon.cpp)

| Deferred variant | Vertex shader and CRC32 | `vehicle_tire` pixel CRC32 | `vehicle_tire_emissive` pixel CRC32 |
| --- | --- | --- | --- |
| Rigid | `VS_VehicleTransformD_0174add1_Wrapped` `0xDF51595B` | `0xDEF27DF8` | `0x1B5B11EC` |
| Rigid, alpha clip | `VS_VehicleTransformD_8b6924fa_Wrapped` `0x2C667625` | `0x5DE20624` | `0x2A35207B` |
| Skinned | `VS_VehicleTransformSkinD_a9226d4c_Wrapped` `0x21145BC8` | `0x00FAFC23` | `0x48718FC0` |
| Skinned, alpha clip | `VS_VehicleTransformSkinD_d258af1e_Wrapped` `0x2E344675` | `0x6DC98463` | `0xF856AEF8` |

For the rigid variants, `matWheelBuffer` at `b11, space1` holds three 64-byte matrices. Both embedded root signatures bind it through graphics root parameter 2, descriptor offset 7 in that table. The skinned variants use `gBoneMtx` at `b0, space0` and `gPreviousBoneMtx` at `b1, space0`, alongside current and previous camera transforms in `g_rage_matrices`.

| Byte offset | Rigid wheel field |
| --- | --- |
| 0 | `matWheelWorld` |
| 64 | `matWheelWorldViewProj` |
| 128 | `matWheelPrevWorldViewProj` |

For `0xDF51595B`, DXIL rows 4–7 of `matWheelBuffer` transform the local tire vertex to `SV_Position`, while rows 8–11 transform it to the previous clip position. The vertex shader passes the previous X and Y scaled by `(0.5, -0.5) * globalScreenSize.xy` and the previous W in `TEXCOORD8`. The paired `0xDEF27DF8` pixel shader divides those X and Y values by W, adds half the screen size, and subtracts `SV_Position.xy`. It writes the two current-to-previous pixel components to `SV_Target4.xy`. The skinned pair `0x21145BC8` / `0x00FAFC23` uses the same output convention with prior bone data.

## Live benchmark evidence

The second trace benchmark recorded 552 draws over 16 FSR dispatches. Every retained draw bound the same resource at `SV_Target4` that GTA passed to the FSR motion input. A selected burst in the city chase produced 60 unique matches among recorded draws from consecutive captures. The trace retrieved its wheel transforms from the CPU-visible `matWheelBuffer` during draw recording.

| Dispatch and wheel draw | Observed world roll from the preceding capture | Stock previous relative to current projected roll | Stock previous origin offset from the preceding capture |
| --- | ---: | ---: | --- |
| `9646`, draw `8`, near left rear tire | −52.46° | −0.007° | `(0.2500, −0.3333)` render px |
| `9647`, draw `8`, near left rear tire | −75.99° | −0.010° | `(−0.5000, 0.5556)` render px |

The opposite rear wheel showed +52.45° and +75.97° in the same intervals. Separate material draws at those positions repeated the same wheel transforms. The stock previous origin offsets match the current-minus-prior FSR jitter to within 0.0001 pixel, so a correction should retain the stock translation row and restore spin in the previous basis. The 14,388-index rear draws share the rigid shader pair `0xDF51595B` / `0xDEF27DF8` and a stable index-buffer range. The CPU snapshots establish the recorded values. They cannot prove that the GPU consumed the same bytes or that each spoke survived occlusion and temporal reconstruction.

The 48-draw capture limit omitted 18 candidates per frame in the chase burst. Its descriptor registry had also reached capacity, so a missing CBV observation cannot establish missing rotation. The positive observations use a verified layout, an observed descriptor, a matching buffer generation, finite transforms and compatible geometry and origins. The revised descriptor cache keeps at most 131,072 entries, reuses storage when it evicts the least recently used entry, and reports normal evictions separately from tracking failures.

## How the correction works

1. Match the byte lengths and CRC32s of the two rigid vertex shaders and their paired standard or emissive pixel shaders in the table above. Verify the original native pipeline and its serialized D3D12 1.1 root signature. The skinned and subsample-alpha variants, unmatched pipelines and unsupported command paths follow the stock path.
2. Clone the verified graphics pipeline with the original vertex and pixel bytecode. Move `b11, space1` from the last range at offset 7 of graphics root table 2 into a dedicated root CBV at parameter 3. For an eligible indexed draw, bind an add-on-owned, 256-byte-aligned upload slice there. The replacement uses the game's index and vertex bindings, render targets, depth state, descriptor heaps and other descriptor tables. It restores the original graphics root signature, pipeline and tables immediately after recording the draw.
3. At native `ExecuteCommandLists`, read the stock wheel constants from the verified CPU-visible source. Match wheel geometry and projected origin to a unique observation from the preceding FSR frame. Fit a rotation around the local X axle in the previous transform's Y/Z basis. Retain the stock previous origin and axle, including the existing projection jitter at that origin. GTA's original shader then writes corrected motion for pixels covered by that draw. The pre-HUD blur samples the resulting FSR motion buffer.

The verified `b11` descriptor is static for the life of its command recording. The runtime assumes GTA finishes writing its CPU-visible constants before `ExecuteCommandLists`. It checks the descriptor generation and source, uses stock constants where a correction cannot be established, and keeps private slices alive through GPU completion and recording replacement. Camera resets, incompatible FSR frames, ambiguous wheel matches and untracked command paths prevent use of the prior observation. The runtime targets the tracked native direct presentation queue. [D3D12 root signature data volatility](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-version-1-1)

## Validation status

The corrected diagnostic trace reached normal exit in two full Story Mode benchmark runs. Those runs establish the original motion target and the recorded missing roll. Three host tests and eight Windows/Proton checks passed, including Device15 descriptor writers and the native submission runtime. The enabled correction completed a full benchmark with at least 59,297 corrected draw submissions and no reported runtime errors. Six consecutive four-frame bursts verified the captured wheel vectors and visible pre-HUD tread blur. See the [wheel correction test report](wheel-correction-test-2026-11-27.md) for measurements and limits.

The later circular-sampling build passes four host and nine Windows/Proton checks, including padded render allocations, separate alternating final depths, native-motion corroboration, early draw recording, and mask lifetime across delayed submissions. Its full Story Mode benchmark exited normally. All 812 observed wheel draws in the 24 captured frames reached the mask, and all 356 validated roll corrections had arc parameters. Same-frame numerical comparisons confirmed that the GPU output follows projected arcs. See the [arc test report](wheel-arc-test-2026-11-27.md) for per-pixel evidence, performance measurements and remaining coverage limits.

## Additional live verification

1. Capture a stationary-camera side view of a wheel with identifiable spokes. Collect consecutive frames during a stationary burnout or when the car's body moves as little as possible. Record the same-frame FSR input, preblur scene, and FSR dimensions and scale. Use distinct regions for the tire or rim, hub, nearby body and background. Compare spokes against their previous-frame positions and separate the spin field from any residual vehicle translation.
2. Trace `SV_Target4` from a confirmed main-scene wheel draw. Record the bound RTV resource and subresource, depth target, viewport, and shader CRCs. Compare its identity with the FSR input resource at the existing dispatch hook. If they differ, trace the copy, resolve or compute pass that produces the FSR input and compare wheel pixels upstream and downstream in the same frame. RenderDoc can inspect Vulkan pixel history under vkd3d-proton. A targeted ReShade trace can also correlate pipeline and render-target binding with the native D3D12 resource that FSR receives. [Mesa RenderDoc instructions](https://docs.mesa3d.org/graphics-debugging/debugging-misrenderings-crashes.html#renderdoc), [ReShade add-on events](https://github.com/crosire/reshade/blob/main/include/reshade_events.hpp)
3. Follow the rendered rim, tire, brake disc and caliper separately. An effect named `vehicle_tire` establishes a shader candidate. Pixel history establishes which visible components draw through it and which pixels remain visible after occlusion.

## Circular wheel sampling

`NativeMotionBlur.fx` exposes **Wheel blur path → Circular arcs** (default) and **Straight comparison**. The correction marker remains required. Circular sampling augments the native-vector correction with an add-on-owned visibility mask, depth texture, and per-draw transform data. Both paths run after FSR and before HUD composition.

The mask replays the verified original wheel shaders into private targets. The original alpha discard and tire deformation remain in effect. The original shaders write alpha 1 to target 2; the private blend state encodes a draw-slot ID there. Private depth chooses the visible wheel surface. The replay restores the game's exact render-target descriptors, depth binding, blend factors, pipeline, and root arguments. Explicit render passes and active queries do not receive this extra draw.

Each slot stores the original matrices, corrected previous matrix, inverse current matrix, fitted roll, frame stamp, and a canonical wheel ID. Material draws with the same transform share a canonical ID. The compute pass maps unjittered FSR output pixels back to the jittered render grid, compares private wheel depth to GTA's final scene depth, and reconstructs the local wheel point. For exposure time `t`, it rotates the local Y/Z coordinates about the X axle by `t × roll`, then projects with a linear interpolation of the current and stock previous transforms. This preserves the exact projected endpoints; the intermediate camera/chassis trajectory is an approximation. Radius limiting shortens `t` on that curve rather than moving samples along a chord.

Every bilinear color contribution must belong to the same canonical wheel, match final scene depth, and have a native motion vector agreeing with its own corrected previous-transform projection. The last test identifies the draw responsible for the texel; it permits the vector direction to change around the rim. Invalid frame stamps, fits, masks, or projections fall back to the ordinary vector path. This is a single-frame gather: it cannot reconstruct hidden spokes, recover full revolutions between observations, or provide physically complete exposure at disocclusions.

Submission-time validation uses finalized CPU-visible wheel constants. Recording admission checks a recently verified FSR target, rather than requiring history or a projected origin that may still be unfinished. The actual substitution still requires a matching submitted frame, geometry, descriptor, unique prior origin, and validated rigid axle fit. Missing or ambiguous history uses the stock constants. Private slots survive command-list reset and draw completion until their exact mask's post-FSR clear has also completed, preventing one frame from reading a later draw's parameters.

The measured GTA depth input is `R32G8X24_TYPELESS`, 1706 × 960, reversed Z, with one sample and one mip. GTA draws wheels against a stable depth resource and records a full copy into alternating FSR depth inputs. The private mask is associated with the original wheel attachment. The compute pass reads the current FSR depth's plane 0 through `R32_FLOAT_X8X24_TYPELESS`, preserving the stencil plane and restoring the original depth-plane state. Copy observations establish the candidate association; per-pixel depth and native-vector checks determine whether the arc shader accepts each contribution. Captures include native and copied-plane formats, original attachment identity, mask IDs, private and scene depths, parameter bytes, and separate coverage-rejection counters.

## Why the straight path can under-blur wheels

The straight comparison gathers along the line defined by the current and previous projected positions. A wheel point travels along an arc, so the projected displacement can be a poor approximation at high angular speed. Two frame orientations also leave any intervening full revolutions ambiguous. The linear gather reduces the weight of samples whose motion differs from the center. For a front-facing wheel undergoing pure rotation by `θ`, a tap at distance `d` differs by `2 × abs(sin(θ / 2)) × d` pixels. At a 180° shutter, with the radius uncapped and the one-pixel tolerance floor inactive, end taps reach zero agreement above 60° per frame. Corrected vectors can therefore produce weaker blur than the motion of the spokes would suggest. [Current gather](../shaders/NativeMotionBlurCS.hlsl), [Epic's rotating-wheel motion-blur analysis](https://dev.epicgames.com/documentation/unreal-engine/using-radial-motion-blur-in-unreal-engine)

## Diagnostic trace ABI fix

The first trace build exited when the wheel capture was armed. Its log ends at the capture request and contains no recorded wheel dispatch. The available evidence includes no crash stack. Compiler output showed an ABI mismatch in the two ReShade virtual methods used to resolve render-target views. The LLVM-MinGW add-on and official MSVC-built ReShade pass the storage for a returned C++ aggregate differently. In an isolated Proton fixture, the MSVC-to-MSVC reference returned the expected handle, while the original MinGW-to-MSVC call exited with code 5 when the descriptor value pointed outside mapped memory.

The corrected observer records view identity from the `init_resource_view` event and looks it up when the game binds the target. A raw `rt4_view` handle with `rt4=0` means the observer did not resolve its resource, which can happen when a descriptor is copied through a path that does not emit the view-creation event. The trace records CPU-visible wheel matrices during draw recording. Use `analyze_wheel_trace.py` to compare the supplied prior matrix with a candidate drawn in the preceding capture. These observations cannot prove which values the GPU consumed. [ReShade D3D12 render-target view callbacks](https://github.com/crosire/reshade/blob/18deaa52de0c425a78b329e9cb3c497281cd00ec/source/d3d12/d3d12_device.cpp#L584-L626)

## Reproducibility

The two archives were read locally with the MIT-licensed [gtav-enhanced-rpf revision `c09f1f1`](https://github.com/wjy000/gtav-enhanced-rpf/tree/c09f1f1b15f7a0ebe77f852c44f8aca3c9e358a7), then inspected using the MIT-licensed [GTA V ShaderManager revision `ffa2298`](https://github.com/crxhvrd/gta5-shadermanager/tree/ffa2298749754559ef191be223305b96a68c0082) and [Microsoft DXC `-dumpbin`](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2505.1). The extracted assets and disassemblies stayed in a local temporary directory. CodeWalkerProjects also supports browsing Enhanced `SGD2` metadata and exporting individual shaders. [CodeWalkerProjects shader libraries](https://github.com/crucialthread/CodeWalkerProjects#part-3--a-new-format-shader-libraries-sga_awc)
