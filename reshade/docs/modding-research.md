# GTA V Enhanced modding research

Research date: 27 November 2026. Target: GTA V Enhanced, single-player Story Mode, Linux/Proton. Primary objective: access engine motion vectors for a real-time per-object motion-blur mod. This is a source-backed investigation and proposed experiment; no game capture or mod was run for this report.

## Recommendation

Start with **native Linux RenderDoc for inspection**, then implement a small **D3D12 hook at the game's temporal-upscaler inputs** to expose motion vectors to a diagnostic shader. Enhanced has a much better starting point than Legacy: OptiScaler's maintainers report working DLSS and FSR 3.1 inputs for Enhanced, and public source code shows how to obtain the underlying motion-vector resource. Their compatibility entry is a Windows test, so it does not by itself establish Proton compatibility. [OptiScaler GTA V Enhanced compatibility](https://github.com/optiscaler/OptiScaler/wiki/Grand-Theft-Auto-V-Enhanced).

There is also direct, though configuration-specific, Proton evidence: an Enhanced user reports 16,200 successful evaluations with the open-source `addon-dlssnr-linux` v0.2.2 on an RTX 4080, GE-Proton11-6 and DLSS Balanced. Its NGX hook and consumer access the game's motion-vector input. This establishes a reported working integration, not visual validation of vector decoding or coverage for every object type. It is a useful source template, not a ready-made motion-blur mod. [Test report](https://github.com/NapXDD/addon-dlssnr-linux/issues/3#issuecomment-5550517967), [versioned interception code](https://github.com/NapXDD/addon-dlssnr-linux/blob/v0.2.2/src/ngx_probe.hpp), [native input consumer](https://github.com/NapXDD/addon-dlssnr-linux/blob/v0.2.2/src/nr_runner.hpp#L553-L625).

The first deliverable should be a **live motion-vector visualization** that distinguishes a moving car and walking pedestrian against a stationary background. Establish vector correctness before implementing blur.

## Existing access methods and evidence

| Project or source | What it establishes | Relevance and limitations |
| --- | --- | --- |
| [OptiScaler FSR input hook](https://github.com/optiscaler/OptiScaler/blob/master/OptiScaler/inputs/FfxApi_Dx12.cpp) | Reads `ffxDispatchDescUpscale.motionVectors.resource`, `motionVectorScale`, `jitterOffset`, depth and render dimensions. | Concrete source for accessing engine-provided inputs. A capture-only adaptation still needs testing with Enhanced's current FSR integration under Proton. |
| [OptiScaler DLSS Motion Vector Extractor](https://github.com/jfbobier/OptiScaler-DLSSMotionVectorExtractor) | A public fork exports `DLSSVEC_SetConsumerV1`; callbacks receive D3D11/12 resources and command context. | Closest reusable extraction interface found. Its default backend uses native NVIDIA DLSS; GTA V Enhanced/Proton operation of this particular fork is unverified. |
| [Extractor provider implementation](https://github.com/jfbobier/OptiScaler-DLSSMotionVectorExtractor/blob/visionalvr/OptiScaler/visionalvr/VaProvider.cpp) | Retrieves `NVSDK_NGX_Parameter_MotionVectors`, depth/color, scale, jitter, input subrects, dimensions and reset/feature flags. | Source confirms the API publishes actual resources, not estimated optical flow. The consumer must implement visualization or blur. |
| [addon-dlssnr-linux](https://github.com/NapXDD/addon-dlssnr-linux) | Public NGX interception and resource inspection, with the Enhanced/Proton report above. | Stronger platform-specific precedent than a generic extractor. Reuse the interception approach; neural rendering is outside this project's objective. |
| [philkr/gamehook_gtav](https://github.com/philkr/gamehook_gtav/blob/1b63aba5c11aa8ddee947cf7ada990a6649b9c79/gta5.cpp) | Historical open code tracks transforms, bones and wheels, modifies shaders, and creates its own `flow` and `object_id` targets. | Valuable reference for generating geometry-based flow when the engine does not expose it. This is 2018-era Legacy code, not an Enhanced buffer address or compatible current plug-in. |

**Answer to “has anyone found access?”:** yes, public projects access the relevant native upscaler inputs, and an adjacent implementation has an Enhanced/Proton success report. A verified, universal GTA V Enhanced motion-vector texture identifier, shader slot, format and decoding recipe was not established. Those must be measured on the actual build. Resource pointers and capture IDs are not stable APIs.

Legacy information must be kept separate. PureDark's 2023 GTA V upscaler demo says it generated motion vectors by patching hundreds of shaders. The older graphics study describes camera-directed blur with stencil masking, not a documented complete velocity target. Neither supports an assertion that today's Enhanced has no vectors. [PureDark's original demonstration](https://www.youtube.com/watch?v=GkG_h6RQvCM), [Adrian Courrèges on GTA V post-effects](https://www.adriancourreges.com/blog/2015/11/02/gta-v-graphics-study-part-3/).

Image-based optical flow, camera motion reconstructed from depth, and geometry motion vectors are different inputs. Optical flow can approximate moving objects but must infer motion from rendered colors; depth plus camera transforms alone cannot recover independently moving cars or limbs. The engine-provided temporal inputs are the appropriate first target.

## Tools and interception layers

The relevant rendering path is:

```text
GTA V Enhanced / Windows D3D12 calls
    ├── FSR or DLSS input hook: native resources + semantic metadata
    └── vkd3d-proton: D3D12 → Vulkan
            └── RenderDoc: Vulkan resources, draws, dispatches and shaders
                    └── Linux GPU driver
```

A Windows x64 DLL inside the Proton process can inspect D3D12 interfaces before translation. Native Linux RenderDoc observes the resulting Vulkan images and commands. These are complementary views; a Vulkan capture does not directly expose the original FidelityFX function arguments. [vkd3d-proton architecture and debugging](https://github.com/HansKristian-Work/vkd3d-proton#readme).

| Tool | Purpose for this project |
| --- | --- |
| [RenderDoc](https://github.com/baldurk/renderdoc) | First choice for frame inspection: texture values, resource usage, producing draws/dispatches, pipeline state and shader inspection. Install the Linux build and enable its Vulkan layer in the game's launch environment. |
| [vkd3d-proton debugging](https://github.com/HansKristian-Work/vkd3d-proton#advanced-shader-debugging) | Dump original DXBC/DXIL and translated SPIR-V; correlate shader hashes with captured passes. Shader dumps alone do not contain texture contents. |
| [ReShade add-on API and examples](https://github.com/crosire/reshade/tree/main/examples) | Host an in-process diagnostic overlay or effect and resource hooks. An ordinary `.fx` preset does not automatically receive GTA's motion-vector texture. |
| [OptiScaler](https://github.com/optiscaler/OptiScaler) | Existing upscaler interception framework; useful for logging native input resources and adapting an extractor. |
| [Nsight Graphics](https://docs.nvidia.com/nsight-graphics/UserGuide/graphics-capture-overview.html) | Alternative Vulkan inspector on supported NVIDIA/Linux configurations. Current Enhanced/Proton capture success is unverified here. |
| [PIX](https://devblogs.microsoft.com/pix/taking-a-capture/) | Windows fallback for inspecting original D3D12 commands if Proton capture becomes the limiting factor. |
| [3Dmigoto](https://github.com/bo3b/3Dmigoto) | Useful for Legacy's D3D11 shader hunting/replacement; not the starting tool for Enhanced's D3D12 path. |

For CPU internals later, Ghidra or a Windows-aware debugger can investigate native handlers and engine structures. They are unnecessary for the first motion-buffer experiment because a known graphics interface already carries the data.

## First inspection experiment

### Establish a reproducible baseline

Record the exact game build, Proton version, GPU/driver, graphics settings, internal render size and output resolution. Use Story Mode with BattlEye disabled through Rockstar's supported setting. [Rockstar BattlEye FAQ](https://support.rockstargames.com/articles/1nenwhZlVrJY6CTFeSS2Fx/grand-theft-auto-online-battleye-faq).

For the initial experiment, disable frame generation, ray tracing and other graphics injectors. Use fixed resolution and an ordinary outdoor scene. Enable FSR upscaling, or DLSS on a compatible NVIDIA setup, to make the temporal-input boundary active. Compare against native AA later; do not assume the same resources persist when changing modes. Turn built-in blur off for the eventual custom-blur comparison, and check whether that setting changes available targets.

### Capture through Proton

With native Linux RenderDoc installed and its Vulkan layer registered, the general Steam launch-option starting point is:

```text
ENABLE_VULKAN_RENDERDOC_CAPTURE=1 %command%
```

Open RenderDoc's **File → Attach to Running Instance** and select the actual game instance that is presenting frames, rather than the Rockstar launcher. Capture after the scene has been moving for several frames. This is a documented Vulkan-layer approach, not a verified one-command recipe for every Enhanced/Proton installation. Steam Runtime or Flatpak packaging must make the layer visible inside the game's environment. [Mesa capture documentation](https://docs.mesa3d.org/graphics-debugging/debugging-misrenderings-crashes.html), [Valve Proton capture guidance](https://partner.steamgames.com/doc/steamhardware/steamframe/debugging?l=latam&language=english).

If the instance appears but reports Vulkan **Not Presenting**, Valve documents adding `EnableFrameEndMarkers=1` before `%command%`. Otherwise distinguish layer-loading failure from replay failure. RenderDoc can change the extensions exposed to the application. A failure involving an unsupported Vulkan extension is not evidence that motion vectors are missing. RenderDoc also limits official support to debugging programs you create; commercial-game capture support should not be assumed. [Valve capture troubleshooting](https://partner.steamgames.com/doc/steamhardware/steamframe/debugging?l=latam&language=english), [RenderDoc support scope](https://github.com/baldurk/renderdoc#readme).

### Identify and validate a candidate

Trace textures read by temporal reconstruction, then follow their resource usage back to the draw or compute pass that wrote them. Use formats such as two-channel floating point only as search clues; there is no verified Enhanced format in this report. Include storage images and intermediate resolve/conversion passes in the search.

Inspect raw signed values or a direction/magnitude visualization. A velocity texture can appear black under an ordinary color preview because values are small or negative. Compare controlled cases:

| Scene | What the test determines |
| --- | --- |
| Fixed camera and static scene | Zero baseline, bias/packing, and residual temporal jitter. |
| Fixed camera with a passing car | Whether independently moving geometry writes vectors. |
| Fixed camera with a pedestrian animation | Whole-actor motion first; then compare swinging limbs against torso/root motion, ideally with a stationary root, to establish skeletal deformation coverage. |
| Camera pan across stationary scenery | Direction, scale and camera-motion inclusion. |
| Rotating wheel and moving foliage | Coverage of local transforms and animated materials. |
| Camera cut, pause, resolution change | History reset and resource recreation behavior. |

Keep a capture, raw texture export and metadata record for each useful case. A texture's appearance is insufficient proof: establish its sign, scale and packing, then verify that decoded vectors reproject identifiable image features toward their locations in the preceding rendered frame, accounting for jitter. An engine intermediate may use a different convention from the FSR input. For quantitative checks, preserve the previous image as well.

### Use shader dumps when needed

Create a writable directory, then add the following variables to the existing launch environment:

```text
VKD3D_SHADER_DUMP_PATH=/tmp/gtav-vkd3d-shaders PROTON_LOG=1 %command%
```

vkd3d-proton names dumps by hash with `.spv`, `.dxbc` or `.dxil` suffixes. If cached translations prevent needed dumps, test `VKD3D_SHADER_CACHE_PATH=0` temporarily. Use `spirv-dis`/SPIRV-Cross for translated shaders and DXC tooling where appropriate for original bytecode. Built-in shader-triggered auto-capture requires a vkd3d-proton build with `-Denable_renderdoc=true`; ordinary external Vulkan-layer capture does not. [Debugging options](https://github.com/HansKristian-Work/vkd3d-proton#advanced-shader-debugging).

## From discovery to a motion-blur mod

The proposed prototype is a small Windows x64 DLL, optionally a ReShade add-on, that intercepts the active upscaler boundary and records a GPU copy or visualization pass. On NVIDIA, the DLSS extractor and Proton-tested NGX hook are useful starting points. On AMD/Intel, investigate the game's FSR input path using OptiScaler as the reference. Do not make native NVIDIA DLSS a prerequisite for an FSR-based implementation.

At the FSR boundary, inspect these fields in the matching API version:

```cpp
dispatchDesc->motionVectors.resource
dispatchDesc->motionVectorScale
dispatchDesc->jitterOffset
dispatchDesc->renderSize
dispatchDesc->depth.resource
dispatchDesc->color.resource
```

They are semantic anchors for resource discovery, not a complete hook implementation. Enhanced has historically used a custom FSR library; verify the loaded DLL, exports and structure version rather than assuming a stock SDK wrapper will load unchanged. [OptiScaler FSR source](https://github.com/optiscaler/OptiScaler/blob/master/OptiScaler/inputs/FfxApi_Dx12.cpp), [historical Enhanced integration note](https://github.com/optiscaler/OptiScaler/wiki/Grand-Theft-Auto-V-Enhanced/ee3b401239bec02e02453ad07e8b20a56190d66a).

Under Proton, a proxy DLL may require a Wine override, for example `WINEDLLOVERRIDES="dxgi=n,b"` when it is actually named `dxgi.dll`. Multiple injectors cannot independently occupy that filename; use documented chaining or a supported alternative. Match the override to the DLL actually installed. [OptiScaler installation](https://github.com/optiscaler/OptiScaler/wiki/Manual-Installation).

Record resource format/extent, input subrect, render/output dimensions, scale, vector direction, jitter convention, reset flags and frame duration. AMD documents FSR inputs as current-to-previous motion, with configurable scale/resolution and jitter handling. FSR also creates internal dilated vectors; those can differ from the application's original vectors. Confirm which stage is being sampled. [FSR upscaler documentation](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/).

Implementation requirements derived from the rendering problem:

- **Keep real-time processing on the GPU.** Copy/sample at a known point in the command stream, manage resource barriers, and preserve the state expected by the game. Use fences for cross-queue work and resource retirement. A valid CPU pointer alone does not establish safe GPU access.
- **Normalize vectors before blurring.** Decode sign, units and jitter once. If vectors represent displacement over frame interval `dt`, a physical exposure `t` gives a blur length proportional to `|motion| × t/dt`. Do not silently mix render-resolution pixels with output-resolution pixels.
- **Align color, depth and velocity.** First prove blur in one resolution domain. A proposed final insertion point is after temporal reconstruction and before HUD composition; locating that point and resolving inputs to matching dimensions is separate work. Prefer scene-linear color where available.
- **Respect silhouettes.** Depth-aware sampling, velocity dilation or tile/neighborhood maximum approaches can reduce missing trails and background bleeding. A simple gather filter remains an approximation; one visible surface's vector cannot reconstruct hidden geometry or every transparent layer.
- **Handle resets and reallocation.** Camera cuts, alt-tab, upscaler changes and resolution changes can invalidate both history and textures. Do not retain unowned game resources indefinitely.
- **Test frame generation later.** Process rendered frames consistently and avoid blurring already generated images a second time.

Per-object blur usually means the velocity field includes both camera and object motion. If the desired style suppresses camera blur, a separate camera-motion estimate from depth and current/previous camera transforms can be subtracted in a consistent convention. That requires camera data beyond a basic velocity texture. An object-ID buffer is only necessary if individual entities need separate selection or settings; it is not a prerequisite for ordinary per-pixel velocity blur.

## Mapping scripts and gameplay APIs

There are three different surfaces to inspect: Rockstar script natives, shipped script/data assets, and internal engine/rendering state. No single native database exposes all three.

| Surface | Tools and useful starting points |
| --- | --- |
| Script natives | [alloc8or Enhanced NativeDB](https://alloc8or.re/gta5/nativedb/enhanced/) and its [machine-readable database](https://github.com/alloc8or/gta5-nativedb-data). Search names, namespaces, hashes and parameters; match the edition/build. |
| Executing script calls | [Script Hook V](https://www.dev-c.com/gtav/scripthookv/) with a Windows x64 C++ `.asi` is the simplest initial runtime choice under Proton. Check supported game builds. |
| C# scripting | [ScriptHookVDotNet Enhanced](https://github.com/Chiheb-Bacha/ScriptHookVDotNetEnhanced) is a separate compatibility choice; .NET Framework setup under Proton adds dependencies. Do not assume an arbitrary upstream/Legacy binary supports Enhanced. |
| Shipped script behavior | [Enhanced decompiled scripts](https://github.com/Tessio/v-decompiled-scripts/tree/master/decompiled%20scripts%20-%20enhanced) reveal native call sites and control flow. They do not reveal the C++ implementation of those natives. |
| Archives and configuration | [Paleto Modkit](https://github.com/the-beper/PaletoModkit) is a native Linux archive/meta tool covering both editions. [CodeWalker](https://github.com/dexyfex/CodeWalker) is useful for RPF/resource inspection. Enhanced archive replacement uses tools such as [OpenRPF](https://www.gta5-mods.com/tools/openrpf-openiv-asi-for-gta-v-enhanced). |
| Runtime internals | Native-handler analysis, symbols inferred from shader reflection, pattern scans and graphics hooks. These require build-specific validation; gameplay scripts do not expose GPU resource handles automatically. |

For an Enhanced ASI loader named `xinput1_4.dll`, the corresponding documented Proton override is `WINEDLLOVERRIDES="xinput1_4=n,b"`. Confirm the loader distributed with the selected package rather than blindly renaming files. [Paleto installation instructions](https://github.com/the-beper/PaletoModkit/blob/main/docs/installation.md).

### Scroll-wheel vehicle control

Three behaviors are possible, with different implementations:

1. **Adjustable speed ceiling:** wheel steps alter `SET_VEHICLE_MAX_SPEED`. Its speed is in m/s, and a nonpositive value restores the default. The driver still uses the accelerator. [Native documentation](https://github.com/citizenfx/natives/blob/master/VEHICLE/SetVehicleMaxSpeed.md).
2. **Cruise control:** wheel steps change a target speed; a controller compares measured speed to the target and requests throttle/brake. Preserve manual braking and disable on vehicle exit. `_SET_CONTROL_NORMAL` provides a native input route to investigate. [Control native](https://github.com/citizenfx/natives/blob/master/PAD/SetControlNormal.md).
3. **Analog-like throttle:** wheel steps change a persistent throttle fraction rather than speed. This most closely substitutes for a controller trigger, although slopes and gear changes still affect speed.

`SET_VEHICLE_FORWARD_SPEED` directly changes velocity and is useful for an intentionally arcade prototype; repeatedly forcing it can interfere with collisions, traction and braking. [Native documentation](https://github.com/citizenfx/natives/blob/master/VEHICLE/SetVehicleForwardSpeed.md).

Check wheel-bound control actions in the current context, or collect `WM_MOUSEWHEEL` and consume events on the game tick. Suppress conflicting weapon/radio actions only while the feature is active. [Control mappings](https://docs.fivem.net/docs/game-references/controls/), [Win32 mouse-wheel event](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-mousewheel).

### Jump height and fall damage

`SET_SUPER_JUMP_THIS_FRAME` enables the built-in super-jump behavior for a frame; it has no height argument. A custom adjustable jump can prototype a one-time vertical velocity/impulse at takeoff, preserving horizontal velocity, but animation and ragdoll behavior need measurement. [Super-jump native](https://github.com/citizenfx/natives/blob/master/MISC/SetSuperJumpThisFrame.md), [entity velocity native](https://github.com/citizenfx/natives/blob/master/ENTITY/SetEntityVelocity.md).

Fall damage is not simply a saved player statistic. FiveM implements fall-damage multiplier and fatal-height controls through its own engine hooks. Those CFX extensions cannot be assumed callable through Story Mode Script Hook V. The same distinction applies to convenient FiveM handling setters. For precise offline fall-damage tuning, inspect and adapt the relevant implementation for the exact Enhanced build; for an initial approximation, experimentally validate landing protection or velocity shaping. [Cfx implementation](https://github.com/citizenfx/fivem/commit/659bf5fe1dcb6e529d06b476dcfba7fcd1971817), [CFX-only handling setter](https://github.com/citizenfx/fivem/blob/master/ext/native-decls/SetVehicleHandlingFloat.md).

## Completion criteria for the first prototype

The first implementation milestone is complete when a diagnostic overlay displays native vectors for a moving car and pedestrian with a fixed camera, the decoding survives a resolution change, and a saved capture identifies the resource producer and consumer. Record the tested build and settings alongside the evidence. Motion blur then becomes a bounded shader/integration task, with wheels, transparencies, occlusion and frame generation treated as explicit coverage tests.
