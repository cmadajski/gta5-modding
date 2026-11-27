# GTA V Enhanced ReShade mod

Native FSR motion-vector diagnostics and motion blur before HUD composition, developed for GTA V Enhanced on Linux/Proton.

In a source checkout, all source, build configuration, shaders, presets, scripts, tests, and project documentation live here. Run the documented build and installer commands from this directory (`cd reshade` from the repository root). Build outputs are written under `reshade/build-*`.

## Research

- [GTA V Enhanced modding research](docs/modding-research.md): motion-vector access, Linux/Proton inspection tools, and gameplay API options.

## Native motion vector overlay

The first implementation is a C++ ReShade add-on and visualization shader for GTA V Enhanced's native FSR motion vectors on Linux/Proton. It targets Steam and AMD GPUs, including the RX 7800 XT. It displays direction/magnitude or signed X/Y, logs input metadata, and marks missing input explicitly.

See [installation, build instructions and validation](docs/native-motion-overlay.md). The Windows build, isolated Proton tests, and live native-vector visualization in the game benchmark have been verified on the RX 7800 XT. See the [local test report](docs/live-test-2026-11-27.md) for scope and results. This is a diagnostic tool for the motion-blur experiment, not the blur effect itself.

## Native motion blur

The native compute pass uses the same per-pixel vectors for a centered blur with adjustable shutter, sampling, and radius. It runs immediately after FSR upscaling, before GTA draws the HUD. `NativeMotionBlur.fx` supplies ReShade controls and leaves the presentation image unchanged. The preset defaults to a 180° shutter, nine samples, and a 12-pixel maximum radius. Enable the blur technique and disable GTA's built-in motion blur. Camera and object motion both contribute.

See [blur installation and controls](docs/native-motion-blur.md) and the [pre-HUD in-game validation](docs/hud-blur-test-2026-11-27.md). The package includes `NativeMotionBlur.ini`, with the vector diagnostic available separately. The [original post-HUD test](docs/blur-test-2026-11-27.md) is retained as historical evidence.
