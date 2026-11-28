# Wheel runtime GPU regression

Build with `NATIVE_MOTION_BUILD_GPU_TEST=ON` and a host DXC binary. If DXC is outside `PATH`, set `NATIVE_MOTION_DXC` to its location. Run `ctest --test-dir build-windows -R '^wheel_runtime_gpu_test$' --output-on-failure`. For a Linux cross-build, use the [Proton test setup](../native_blur_gpu/README.md).

This fixture uses authored HLSL and supplies one synthetic PSO identity. It runs the real root-CBV relocation, descriptor tracking, native queue hook, and upload retirement logic without a running game or game shader files.

The test clears startup history on a worker thread before the first real present. It writes the original wheel CBV after command-list recording and verifies that the rendered correction uses the prior frame's submitted orientation while preserving the freshly finalized world and current values. A provisional origin that only becomes valid at submission receives correction. A mismatched submitted origin preserves stock motion and appears in the frame's coverage report. A replay restores stock motion. A deliberately stalled queue checks that a subsequent replay waits for the previous GPU read of the private upload.

For the arc path, the fixture reads an ID from the rendered GPU mask and checks the corresponding 512-byte upload. A replay before consumption preserves those bytes. The slot stays allocated after the wheel list is replaced and after a present without a confirmed clear. The slot becomes reusable when the corresponding mask-clear fence completes.
