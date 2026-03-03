//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  This file implements the OpenXR Loader ↔ Runtime protocol and all
//  XrInstance-level API calls.  XrSession-level calls delegate to session.cpp.

#include "pch.h"
#include "runtime.h"
#include "session.h"
#include "logging.h"
#include "timing.h"
#include <algorithm>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Loader negotiation — first function the loader calls
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrNegotiateLoaderRuntimeInterface(
    const XrNegotiateLoaderInfo* loaderInfo,
    XrNegotiateRuntimeRequest*   runtimeRequest)
{
    if (!loaderInfo || !runtimeRequest)
        return XR_ERROR_INITIALIZATION_FAILED;

    // Validate loader
    if (loaderInfo->structType    != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO    ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION              ||
        loaderInfo->structSize    != sizeof(XrNegotiateLoaderInfo))
        return XR_ERROR_INITIALIZATION_FAILED;

    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;

    // Fill in our capabilities
    runtimeRequest->structType        = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    runtimeRequest->structVersion     = XR_RUNTIME_INFO_STRUCT_VERSION;
    runtimeRequest->structSize        = sizeof(XrNegotiateRuntimeRequest);
    runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    runtimeRequest->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 0);
    runtimeRequest->getInstanceProcAddr = xrGetInstanceProcAddr;

    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Supported extensions
// ---------------------------------------------------------------------------
static const char* const kSupportedExtensions[] = {
    "XR_KHR_D3D11_enable",
};
static const uint32_t kSupportedExtCount =
    static_cast<uint32_t>(std::size(kSupportedExtensions));

// ---------------------------------------------------------------------------
// xrEnumerateApiLayerProperties
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrEnumerateApiLayerProperties(uint32_t propertyCapacityInput,
                               uint32_t* propertyCountOutput,
                               XrApiLayerProperties* /*properties*/)
{
    *propertyCountOutput = 0; // This runtime exposes no API layers
    if (propertyCapacityInput > 0) return XR_SUCCESS;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// xrEnumerateInstanceExtensionProperties
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrEnumerateInstanceExtensionProperties(const char* /*layerName*/,
                                        uint32_t cap,
                                        uint32_t* count,
                                        XrExtensionProperties* props)
{
    *count = kSupportedExtCount;
    if (cap == 0) return XR_SUCCESS;
    if (cap < kSupportedExtCount) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < kSupportedExtCount; ++i) {
        props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        props[i].next = nullptr;
        strncpy_s(props[i].extensionName, kSupportedExtensions[i],
                  XR_MAX_EXTENSION_NAME_SIZE - 1);
        props[i].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// xrCreateInstance
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrCreateInstance(const XrInstanceCreateInfo* createInfo, XrInstance* instance)
{
    if (!createInfo || !instance) return XR_ERROR_VALIDATION_FAILURE;
    if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO)
        return XR_ERROR_VALIDATION_FAILURE;

    // Verify that the application only requests extensions we support
    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i) {
        bool found = false;
        for (uint32_t j = 0; j < kSupportedExtCount; ++j) {
            if (strcmp(createInfo->enabledExtensionNames[i],
                        kSupportedExtensions[j]) == 0) {
                found = true; break;
            }
        }
        if (!found) {
            LOG_ERROR("xrCreateInstance: unsupported extension requested: %s",
                       createInfo->enabledExtensionNames[i]);
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
    }

    xr3dv::Runtime* rt = nullptr;
    if (!xr3dv::RuntimeInit(&rt)) return XR_ERROR_INITIALIZATION_FAILED;

    // Use the runtime pointer itself as the XrInstance handle
    *instance = reinterpret_cast<XrInstance>(rt);
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// xrDestroyInstance
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrDestroyInstance(XrInstance /*instance*/)
{
    xr3dv::RuntimeDestroy();
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// xrGetInstanceProperties
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetInstanceProperties(XrInstance /*instance*/,
                          XrInstanceProperties* props)
{
    props->type = XR_TYPE_INSTANCE_PROPERTIES;
    props->runtimeVersion = XR_MAKE_VERSION(
        XR3DV_VERSION_MAJOR, XR3DV_VERSION_MINOR, XR3DV_VERSION_PATCH);
    strncpy_s(props->runtimeName, "XR3DV (NVIDIA 3D Vision)",
              XR_MAX_RUNTIME_NAME_SIZE - 1);
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// xrGetSystem / xrGetSystemProperties
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetSystem(XrInstance /*instance*/,
             const XrSystemGetInfo* info,
             XrSystemId* systemId)
{
    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    *systemId = 1; // single system
    return XR_SUCCESS;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetSystemProperties(XrInstance /*instance*/,
                       XrSystemId /*systemId*/,
                       XrSystemProperties* props)
{
    props->systemId = 1;
    props->vendorId = 0x10DE; // NVIDIA
    strncpy_s(props->systemName, "XR3DV / NVIDIA 3D Vision",
              XR_MAX_SYSTEM_NAME_SIZE - 1);

    xr3dv::Runtime* rt = xr3dv::GetRuntime();
    props->graphicsProperties.maxSwapchainImageWidth  = rt ? rt->cfg.width  : 1920;
    props->graphicsProperties.maxSwapchainImageHeight = rt ? rt->cfg.height : 1080;
    props->graphicsProperties.maxLayerCount           = 1;

    // No tracking capabilities
    props->trackingProperties.orientationTracking = XR_FALSE;
    props->trackingProperties.positionTracking    = XR_FALSE;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// xrPollEvent
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrPollEvent(XrInstance /*instance*/, XrEventDataBuffer* eventData)
{
    xr3dv::Runtime* rt = xr3dv::GetRuntime();
    if (!rt) return XR_EVENT_UNAVAILABLE;

    // Check if any session needs a state change event delivered
    std::lock_guard<std::mutex> lk(rt->sessionMtx);
    for (auto& [handle, sess] : rt->sessions) {
        XrSessionState st = sess->State();
        if (st == XR_SESSION_STATE_EXITING) {
            auto* ev = reinterpret_cast<XrEventDataSessionStateChanged*>(eventData);
            ev->type    = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
            ev->next    = nullptr;
            ev->session = reinterpret_cast<XrSession>(handle);
            ev->state   = XR_SESSION_STATE_EXITING;
            ev->time    = xr3dv::FrameTimer::NowNs();
            return XR_SUCCESS;
        }
    }
    return XR_EVENT_UNAVAILABLE;
}

// ---------------------------------------------------------------------------
// xrResultToString / xrStructureTypeToString  (minimal)
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrResultToString(XrInstance /*instance*/, XrResult value, char resultString[XR_MAX_RESULT_STRING_SIZE])
{
    snprintf(resultString, XR_MAX_RESULT_STRING_SIZE, "XrResult(%d)", value);
    return XR_SUCCESS;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrStructureTypeToString(XrInstance /*instance*/, XrStructureType value,
                          char typeString[XR_MAX_STRUCTURE_NAME_SIZE])
{
    snprintf(typeString, XR_MAX_STRUCTURE_NAME_SIZE, "XrStructureType(%d)", value);
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// View configuration
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrEnumerateViewConfigurations(XrInstance /*instance*/, XrSystemId /*systemId*/,
                               uint32_t cap, uint32_t* count,
                               XrViewConfigurationType* types)
{
    *count = 1;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 1)  return XR_ERROR_SIZE_INSUFFICIENT;
    types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetViewConfigurationProperties(XrInstance /*instance*/, XrSystemId /*systemId*/,
                                   XrViewConfigurationType type,
                                   XrViewConfigurationProperties* props)
{
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    props->viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    props->fovMutable            = XR_FALSE;
    return XR_SUCCESS;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrEnumerateViewConfigurationViews(XrInstance /*instance*/, XrSystemId /*systemId*/,
                                   XrViewConfigurationType type,
                                   uint32_t cap, uint32_t* count,
                                   XrViewConfigurationView* views)
{
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;

    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2)  return XR_ERROR_SIZE_INSUFFICIENT;

    xr3dv::Runtime* rt = xr3dv::GetRuntime();
    uint32_t w = rt ? rt->cfg.width  : 1920;
    uint32_t h = rt ? rt->cfg.height : 1080;

    for (int i = 0; i < 2; ++i) {
        views[i].type                            = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        views[i].next                            = nullptr;
        views[i].recommendedImageRectWidth       = w;
        views[i].maxImageRectWidth               = w;
        views[i].recommendedImageRectHeight      = h;
        views[i].maxImageRectHeight              = h;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount         = 1;
    }
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// D3D11 graphics requirements (XR_KHR_D3D11_enable)
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetD3D11GraphicsRequirementsKHR(XrInstance /*instance*/,
                                   XrSystemId /*systemId*/,
                                   XrGraphicsRequirementsD3D11KHR* req)
{
    req->type               = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
    req->adapterLuid        = {}; // zero = any adapter
    req->minFeatureLevel    = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Session management
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrCreateSession(XrInstance /*instance*/,
                 const XrSessionCreateInfo* createInfo,
                 XrSession* session)
{
    xr3dv::Runtime* rt = xr3dv::GetRuntime();
    if (!rt) return XR_ERROR_RUNTIME_FAILURE;

    // Find D3D11 graphics binding in the next chain
    const XrGraphicsBindingD3D11KHR* binding = nullptr;
    const XrBaseInStructure* next =
        reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
    while (next) {
        if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            binding = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next);
            break;
        }
        next = next->next;
    }

    if (!binding) {
        LOG_ERROR("xrCreateSession: only XR_KHR_D3D11_enable is supported");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }

    auto sess = std::make_unique<xr3dv::Session>(rt->cfg);
    XrResult res = sess->InitD3D11(binding);
    if (res != XR_SUCCESS) return res;

    std::lock_guard<std::mutex> lk(rt->sessionMtx);
    uint64_t h = rt->nextSessionHandle++;
    rt->sessions[h] = std::move(sess);
    *session = reinterpret_cast<XrSession>(h);
    return XR_SUCCESS;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrDestroySession(XrSession session)
{
    xr3dv::Runtime* rt = xr3dv::GetRuntime();
    if (!rt) return XR_ERROR_RUNTIME_FAILURE;
    std::lock_guard<std::mutex> lk(rt->sessionMtx);
    auto h = reinterpret_cast<uint64_t>(session);
    if (rt->sessions.erase(h) == 0) return XR_ERROR_HANDLE_INVALID;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Delegate session methods
// ---------------------------------------------------------------------------
#define GET_SESSION(s) \
    xr3dv::Runtime* rt = xr3dv::GetRuntime(); \
    if (!rt) return XR_ERROR_RUNTIME_FAILURE; \
    xr3dv::Session* sess = rt->GetSession(s); \
    if (!sess) return XR_ERROR_HANDLE_INVALID

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrBeginSession(XrSession session, const XrSessionBeginInfo* info)
{
    GET_SESSION(session); return sess->Begin(info);
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrEndSession(XrSession session)
{
    GET_SESSION(session); return sess->End();
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrRequestExitSession(XrSession session)
{
    GET_SESSION(session); return sess->RequestExit();
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state)
{
    GET_SESSION(session); return sess->WaitFrame(info, state);
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrBeginFrame(XrSession session, const XrFrameBeginInfo* info)
{
    GET_SESSION(session); return sess->BeginFrame(info);
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrEndFrame(XrSession session, const XrFrameEndInfo* info)
{
    GET_SESSION(session); return sess->EndFrame(info);
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrLocateViews(XrSession session, const XrViewLocateInfo* info,
              XrViewState* viewState, uint32_t cap, uint32_t* count, XrView* views)
{
    GET_SESSION(session);
    return sess->LocateViews(info, viewState, cap, count, views);
}

// ---- Swapchains ----------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrEnumerateSwapchainFormats(XrSession session, uint32_t cap, uint32_t* count, int64_t* fmts)
{
    GET_SESSION(session);
    return sess->EnumerateSwapchainFormats(cap, count, fmts);
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* ci, XrSwapchain* out)
{
    GET_SESSION(session); return sess->CreateSwapchain(ci, out);
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrDestroySwapchain(XrSwapchain swapchain)
{
    xr3dv::Runtime* rt = xr3dv::GetRuntime();
    if (!rt) return XR_ERROR_RUNTIME_FAILURE;
    // Find the session that owns this swapchain
    std::lock_guard<std::mutex> lk(rt->sessionMtx);
    for (auto& [h, sess] : rt->sessions) {
        if (sess->GetSwapchain(swapchain)) return sess->DestroySwapchain(swapchain);
    }
    return XR_ERROR_HANDLE_INVALID;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t cap, uint32_t* count,
                            XrSwapchainImageBaseHeader* images)
{
    xr3dv::Runtime* rt = xr3dv::GetRuntime();
    if (!rt) return XR_ERROR_RUNTIME_FAILURE;

    xr3dv::Swapchain* sc = nullptr;
    {
        std::lock_guard<std::mutex> lk(rt->sessionMtx);
        for (auto& [h, sess] : rt->sessions) {
            sc = sess->GetSwapchain(swapchain);
            if (sc) break;
        }
    }
    if (!sc) return XR_ERROR_HANDLE_INVALID;

    uint32_t n = xr3dv::Swapchain::kImageCount;
    *count = n;
    if (cap == 0) return XR_SUCCESS;
    if (cap < n)  return XR_ERROR_SIZE_INSUFFICIENT;

    auto* d3d11Images = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
    for (uint32_t i = 0; i < n; ++i) {
        d3d11Images[i].type    = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        d3d11Images[i].next    = nullptr;
        d3d11Images[i].texture = sc->Images()[i].tex.Get();
    }
    return XR_SUCCESS;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrAcquireSwapchainImage(XrSwapchain swapchain,
                          const XrSwapchainImageAcquireInfo* /*info*/,
                          uint32_t* index)
{
    xr3dv::Runtime* rt = xr3dv::GetRuntime();
    if (!rt) return XR_ERROR_RUNTIME_FAILURE;
    std::lock_guard<std::mutex> lk(rt->sessionMtx);
    for (auto& [h, sess] : rt->sessions) {
        auto* sc = sess->GetSwapchain(swapchain);
        if (sc) return sc->Acquire(*index);
    }
    return XR_ERROR_HANDLE_INVALID;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrWaitSwapchainImage(XrSwapchain swapchain,
                      const XrSwapchainImageWaitInfo* info)
{
    xr3dv::Runtime* rt = xr3dv::GetRuntime();
    if (!rt) return XR_ERROR_RUNTIME_FAILURE;
    std::lock_guard<std::mutex> lk(rt->sessionMtx);
    for (auto& [h, sess] : rt->sessions) {
        auto* sc = sess->GetSwapchain(swapchain);
        if (sc) return sc->Wait(*info);
    }
    return XR_ERROR_HANDLE_INVALID;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrReleaseSwapchainImage(XrSwapchain swapchain,
                          const XrSwapchainImageReleaseInfo* /*info*/)
{
    xr3dv::Runtime* rt = xr3dv::GetRuntime();
    if (!rt) return XR_ERROR_RUNTIME_FAILURE;
    std::lock_guard<std::mutex> lk(rt->sessionMtx);
    for (auto& [h, sess] : rt->sessions) {
        auto* sc = sess->GetSwapchain(swapchain);
        if (sc) return sc->Release();
    }
    return XR_ERROR_HANDLE_INVALID;
}

// ---------------------------------------------------------------------------
// xrGetInstanceProcAddr — function pointer dispatch table
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetInstanceProcAddr(XrInstance /*instance*/, const char* name,
                       PFN_xrVoidFunction* function)
{
#define DISPATCH(fn) if (strcmp(name, #fn) == 0) { *function = reinterpret_cast<PFN_xrVoidFunction>(fn); return XR_SUCCESS; }
    DISPATCH(xrGetInstanceProcAddr)
    DISPATCH(xrEnumerateApiLayerProperties)
    DISPATCH(xrEnumerateInstanceExtensionProperties)
    DISPATCH(xrCreateInstance)
    DISPATCH(xrDestroyInstance)
    DISPATCH(xrGetInstanceProperties)
    DISPATCH(xrPollEvent)
    DISPATCH(xrResultToString)
    DISPATCH(xrStructureTypeToString)
    DISPATCH(xrGetSystem)
    DISPATCH(xrGetSystemProperties)
    DISPATCH(xrCreateSession)
    DISPATCH(xrDestroySession)
    DISPATCH(xrEnumerateReferenceSpaces)
    DISPATCH(xrCreateReferenceSpace)
    DISPATCH(xrGetReferenceSpaceBoundsRect)
    DISPATCH(xrCreateActionSpace)
    DISPATCH(xrLocateSpace)
    DISPATCH(xrDestroySpace)
    DISPATCH(xrEnumerateViewConfigurations)
    DISPATCH(xrGetViewConfigurationProperties)
    DISPATCH(xrEnumerateViewConfigurationViews)
    DISPATCH(xrEnumerateSwapchainFormats)
    DISPATCH(xrCreateSwapchain)
    DISPATCH(xrDestroySwapchain)
    DISPATCH(xrEnumerateSwapchainImages)
    DISPATCH(xrAcquireSwapchainImage)
    DISPATCH(xrWaitSwapchainImage)
    DISPATCH(xrReleaseSwapchainImage)
    DISPATCH(xrBeginSession)
    DISPATCH(xrEndSession)
    DISPATCH(xrRequestExitSession)
    DISPATCH(xrWaitFrame)
    DISPATCH(xrBeginFrame)
    DISPATCH(xrEndFrame)
    DISPATCH(xrLocateViews)
    DISPATCH(xrStringToPath)
    DISPATCH(xrPathToString)
    DISPATCH(xrCreateActionSet)
    DISPATCH(xrDestroyActionSet)
    DISPATCH(xrCreateAction)
    DISPATCH(xrDestroyAction)
    DISPATCH(xrSuggestInteractionProfileBindings)
    DISPATCH(xrAttachSessionActionSets)
    DISPATCH(xrGetCurrentInteractionProfile)
    DISPATCH(xrGetActionStateBoolean)
    DISPATCH(xrGetActionStateFloat)
    DISPATCH(xrGetActionStateVector2f)
    DISPATCH(xrGetActionStatePose)
    DISPATCH(xrSyncActions)
    DISPATCH(xrEnumerateBoundSourcesForAction)
    DISPATCH(xrGetInputSourceLocalizedName)
    DISPATCH(xrApplyHapticFeedback)
    DISPATCH(xrStopHapticFeedback)
    DISPATCH(xrGetD3D11GraphicsRequirementsKHR)
#undef DISPATCH

    *function = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// DLL entry point
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE /*hinstDLL*/, DWORD fdwReason, LPVOID /*lpvReserved*/)
{
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(nullptr);
            break;
        case DLL_PROCESS_DETACH:
            xr3dv::RuntimeDestroy();
            break;
    }
    return TRUE;
}
