#include "fsr_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <windows.h>
#include <ffx_api/dx12/ffx_api_dx12.h>
#include <ffx_api/ffx_upscale.h>

namespace
{
void check(bool success, const char *step)
{
    if (!success)
    {
        std::fprintf(stderr, "%s failed (Win32 error %lu)\n", step, GetLastError());
        std::exit(EXIT_FAILURE);
    }
}

template <typename Function>
Function load_export(HMODULE library, const char *name)
{
    const auto address = GetProcAddress(library, name);
    check(address != nullptr, name);
    return reinterpret_cast<Function>(address);
}

// Only GetDesc is called by the probe. The other COM methods let this test run
// without creating a D3D12 device or depending on the host GPU.
struct MockTexture final : ID3D12Resource
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Map(UINT, const D3D12_RANGE *, void **) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE Unmap(UINT, const D3D12_RANGE *) override {}
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    D3D12_RESOURCE_DESC *STDMETHODCALLTYPE GetDesc(D3D12_RESOURCE_DESC *output) override
    {
        *output = description;
        return output;
    }
#else
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return description; }
#endif
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { return 0; }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX *, const void *, UINT, UINT) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void *, UINT, UINT, UINT, const D3D12_BOX *) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS *) override
    {
        return E_NOTIMPL;
    }

    D3D12_RESOURCE_DESC description{};
};

unsigned (*provider_dispatch_count)();
unsigned dispatches_at_capture;
unsigned dispatches_at_post;
unsigned capture_count;
unsigned post_count;
fsr_probe::Sample latest_sample;
fsr_probe::Sample latest_post_sample;

void capture(const fsr_probe::Sample &sample) noexcept
{
    ++capture_count;
    latest_sample = sample;
    dispatches_at_capture = provider_dispatch_count();
}

void post_dispatch(const fsr_probe::Sample &sample) noexcept
{
    ++post_count;
    latest_post_sample = sample;
    dispatches_at_post = provider_dispatch_count();
}
} // namespace

int main()
{
    using fsr_probe::InstallResult;
    check(fsr_probe::try_install(capture) == InstallResult::waiting_for_module, "Wait for the provider to load");
    check(fsr_probe::try_install(capture, true) == InstallResult::waiting_for_module,
          "Do not preload the provider outside GTA V Enhanced");
    const auto provider = LoadLibraryExW(L"amd_fidelityfx_dx12.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    check(provider != nullptr, "Load the test provider from the executable directory");
    const auto create = load_export<PfnFfxCreateContext>(provider, "ffxCreateContext");
    const auto destroy = load_export<PfnFfxDestroyContext>(provider, "ffxDestroyContext");
    const auto dispatch = load_export<PfnFfxDispatch>(provider, "ffxDispatch");
    provider_dispatch_count = load_export<decltype(provider_dispatch_count)>(provider, "TestDispatchCount");

    ffxCreateBackendDX12Desc dx12{};
    dx12.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    ffxCreateContextDescUpscale settings{};
    settings.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    settings.header.pNext = &dx12.header;
    settings.maxUpscaleSize = {1920, 1080};
    settings.flags = FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
                     FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    ffxContext context = nullptr;
    check(create(&context, &settings.header, nullptr) == FFX_API_RETURN_OK, "Create a context before attachment");
    check(fsr_probe::try_install(capture, false, post_dispatch) == InstallResult::installed, "Attach the probe");

    MockTexture motion;
    motion.description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    motion.description.Width = 1920;
    motion.description.Height = 1080;
    motion.description.DepthOrArraySize = 1;
    motion.description.MipLevels = 1;
    motion.description.SampleDesc.Count = 1;
    motion.description.Format = DXGI_FORMAT_R16G16_FLOAT;
    MockTexture output;
    output.description = motion.description;
    output.description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    int command_token = 0;
    ffxDispatchDescUpscale frame{};
    frame.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    frame.commandList = &command_token;
    frame.motionVectors.resource = &motion;
    frame.motionVectors.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    frame.motionVectors.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    frame.output.resource = &output;
    frame.output.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    frame.output.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    frame.renderSize = {1280, 720};
    frame.motionVectorScale = {1920.0f, -1080.0f};
    frame.jitterOffset = {0.25f, -0.125f};
    frame.frameTimeDelta = 12.5f;
    frame.reset = true;

    check(dispatch(&context, &frame.header) == FFX_API_RETURN_OK, "Forward an untracked context");
    check(capture_count == 0 && post_count == 0 && fsr_probe::get_stats().untracked_contexts == 1,
          "Skip the untracked context");
    check(destroy(&context, nullptr) == FFX_API_RETURN_OK, "Forward context destruction");

    settings.header.pNext = nullptr;
    check(create(&context, &settings.header, nullptr) == FFX_API_RETURN_OK, "Forward non-DX12 context creation");
    check(dispatch(&context, &frame.header) == FFX_API_RETURN_OK, "Forward a non-DX12 context");
    check(capture_count == 0 && post_count == 0 && fsr_probe::get_stats().untracked_contexts == 2,
          "Skip the non-DX12 context");
    check(destroy(&context, nullptr) == FFX_API_RETURN_OK, "Forward non-DX12 context destruction");

    settings.header.pNext = &dx12.header;
    check(create(&context, &settings.header, nullptr) == FFX_API_RETURN_OK, "Forward DX12 context creation");
    frame.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK;
    check(dispatch(&context, &frame.header) == FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE, "Forward an unrelated descriptor");
    check(capture_count == 0 && post_count == 0 && fsr_probe::get_stats().upscale_dispatches == 2,
          "Skip the unrelated descriptor");

    frame.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    frame.motionVectors.state = FFX_API_RESOURCE_STATE_COMMON;
    check(dispatch(&context, &frame.header) == FFX_API_RETURN_OK, "Forward an unsupported state");
    check(capture_count == 0 && post_count == 0 && fsr_probe::get_stats().unsupported_samples == 1,
          "Skip the unsupported state");

    frame.motionVectors.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    frame.upscaleSize = {2048, 1080};
    check(dispatch(&context, &frame.header) == FFX_API_RETURN_OK, "Forward an out-of-bounds extent");
    check(capture_count == 0 && post_count == 0 && fsr_probe::get_stats().unsupported_samples == 2,
          "Skip the out-of-bounds extent");

    frame.upscaleSize = {};
    const auto previous_dispatches = provider_dispatch_count();
    check(dispatch(&context, &frame.header) == FFX_API_RETURN_OK, "Forward a supported dispatch");
    check(capture_count == 1 && fsr_probe::get_stats().samples == 1, "Capture the supported dispatch once");
    check(dispatches_at_capture == previous_dispatches && provider_dispatch_count() == previous_dispatches + 1 &&
              post_count == 1 && dispatches_at_post == previous_dispatches + 1 &&
              latest_sample.dispatch_id != 0 && latest_post_sample.dispatch_id == latest_sample.dispatch_id,
          "Call each consumer around the single original dispatch");
    check(!latest_sample.output_valid && latest_sample.output == nullptr, "Keep output internal before FidelityFX succeeds");
    check(latest_post_sample.output_valid && latest_post_sample.output == &output &&
              latest_post_sample.output_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
              latest_post_sample.output_texture.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
              latest_post_sample.output_texture.Width == 1920 && latest_post_sample.output_texture.Height == 1080,
          "Publish the native output only after the original dispatch");
    check(latest_sample.source_state == (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
          "Translate the resource state");
    check(latest_sample.motion_vectors == &motion && latest_sample.command_list == static_cast<void *>(&command_token),
          "Forward the native pointers");
    check(latest_sample.texture.Format == DXGI_FORMAT_R16G16_FLOAT && latest_sample.texture.Width == 1920 &&
              latest_sample.texture.Height == 1080,
          "Read the native texture description");
    check(latest_sample.display_resolution_motion_vectors && latest_sample.jitter_cancellation && latest_sample.reset,
          "Forward the context and dispatch flags");
    check(latest_sample.frame_time_ms == 12.5f, "Forward the frame time in milliseconds");
    check(latest_sample.upscale_width == 1920 && latest_sample.upscale_height == 1080 &&
              latest_sample.render_width == 1280 && latest_sample.render_height == 720,
          "Forward render extent and fall back to the context's output extent");
    check(latest_sample.scale_x == 1920.0f && latest_sample.scale_y == -1080.0f &&
              latest_sample.jitter_x == 0.25f && latest_sample.jitter_y == -0.125f,
          "Forward vector scale and jitter metadata");

    const auto first_dispatch_id = latest_post_sample.dispatch_id;
    frame.sharpness = -1.0f;
    check(dispatch(&context, &frame.header) == FFX_API_RETURN_ERROR_PARAMETER &&
              provider_dispatch_count() == previous_dispatches + 2 && capture_count == 2 && post_count == 1 &&
              latest_sample.dispatch_id == first_dispatch_id + 1 && latest_post_sample.dispatch_id == first_dispatch_id,
          "Forward provider failure and skip the post callback");
    frame.sharpness = 0.0f;

    output.description.Width = 1919;
    check(dispatch(&context, &frame.header) == FFX_API_RETURN_OK && capture_count == 3 && post_count == 2 &&
              !latest_post_sample.output_valid && latest_post_sample.output == nullptr &&
              latest_post_sample.dispatch_id == first_dispatch_id + 2 && latest_post_sample.dispatch_id == latest_sample.dispatch_id &&
              fsr_probe::get_stats().unsupported_samples == 2,
          "Provide vector diagnostics and mark the undersized output unusable");
    output.description.Width = 1920;
    frame.output.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;

    auto old_handle = context;
    const auto old_context_id = latest_sample.context_id;
    check(destroy(&context, nullptr) == FFX_API_RETURN_OK && context == nullptr, "Destroy the tracked context");
    check(dispatch(&old_handle, &frame.header) == FFX_API_RETURN_OK && capture_count == 3 && post_count == 2,
          "Stop publishing for a destroyed context");
    check(create(&context, &settings.header, nullptr) == FFX_API_RETURN_OK && context == old_handle,
          "Create a new context with a recycled handle");
    check(dispatch(&context, &frame.header) == FFX_API_RETURN_OK && capture_count == 4 && post_count == 3 &&
              latest_sample.context_id != old_context_id && latest_post_sample.context_id == latest_sample.context_id &&
              latest_post_sample.output_valid && latest_post_sample.output_state == latest_sample.source_state,
          "Give the recycled handle its own identity and translate a shader-readable output state");

    fsr_probe::stop_capture();
    check(dispatch(&context, &frame.header) == FFX_API_RETURN_OK && capture_count == 4 && post_count == 3,
          "Stop both callbacks while preserving dispatch forwarding");
    check(destroy(&context, nullptr) == FFX_API_RETURN_OK, "Preserve context destruction after stopping");
    std::puts("FidelityFX probe integration checks passed");
}
