#include <ffx_api/ffx_upscale.h>

namespace
{
int context_handle;
unsigned dispatch_count;
}

ffxReturnCode_t ffxCreateContext(ffxContext *context, ffxCreateContextDescHeader *desc,
                                const ffxAllocationCallbacks *)
{
    if (context == nullptr || desc == nullptr || desc->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE)
        return FFX_API_RETURN_ERROR_PARAMETER;
    // Deliberately reuse this handle across creations to exercise probe identity.
    *context = &context_handle;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxDestroyContext(ffxContext *context, const ffxAllocationCallbacks *)
{
    if (context == nullptr || *context != &context_handle)
        return FFX_API_RETURN_ERROR_PARAMETER;
    *context = nullptr;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxDispatch(ffxContext *context, const ffxDispatchDescHeader *desc)
{
    if (context == nullptr || *context != &context_handle || desc == nullptr)
        return FFX_API_RETURN_ERROR_PARAMETER;
    ++dispatch_count;
    if (desc->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE)
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    const auto *upscale = reinterpret_cast<const ffxDispatchDescUpscale *>(desc);
    return upscale->sharpness < 0.0f ? FFX_API_RETURN_ERROR_PARAMETER : FFX_API_RETURN_OK;
}

extern "C" __declspec(dllexport) unsigned TestDispatchCount()
{
    return dispatch_count;
}
