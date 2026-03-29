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
// Minimal OpenXR graphics-binding structs for extension stubs.
// We deliberately avoid including d3d12.h / vulkan.h / GL headers — XR3DV
// only presents via D3D11. These structs match the OpenXR spec layout exactly.
// ---------------------------------------------------------------------------
#ifndef XR_TYPE_GRAPHICS_BINDING_D3D12_KHR
#  define XR_TYPE_GRAPHICS_BINDING_D3D12_KHR    ((XrStructureType)1000028000)
#endif
#ifndef XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR
#  define XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR ((XrStructureType)1000028001)
#endif
#ifndef XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR
#  define XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR     ((XrStructureType)1000028002)
#endif

// D3D12 binding — void* for device/queue avoids pulling in d3d12.h here
// (Session.cpp includes d3d12.h and casts appropriately)
typedef struct XrGraphicsBindingD3D12KHR {
    XrStructureType type;
    void*           next;
    void*           device;  // ID3D12Device*
    void*           queue;   // ID3D12CommandQueue*
} XrGraphicsBindingD3D12KHR;

typedef struct XrSwapchainImageD3D12KHR {
    XrStructureType type;
    void*           next;
    void*           texture; // ID3D12Resource*
} XrSwapchainImageD3D12KHR;

typedef struct XrGraphicsRequirementsD3D12KHR {
    XrStructureType   type;
    void*             next;
    LUID              adapterLuid;       // from windows.h, already included
    D3D_FEATURE_LEVEL minFeatureLevel;   // from d3dcommon.h via d3d11.h
} XrGraphicsRequirementsD3D12KHR;

#ifndef XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR
#  define XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR  ((XrStructureType)1000023001)
#endif
typedef struct XrGraphicsRequirementsOpenGLKHR {
    XrStructureType type;
    void*           next;
    XrVersion       minApiVersionSupported;
    XrVersion       maxApiVersionSupported;
} XrGraphicsRequirementsOpenGLKHR;

#ifndef XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR
#  define XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR  ((XrStructureType)1000025001)
#endif
typedef struct XrGraphicsRequirementsVulkanKHR {
    XrStructureType type;
    void*           next;
    XrVersion       minApiVersionSupported;
    XrVersion       maxApiVersionSupported;
} XrGraphicsRequirementsVulkanKHR;

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
struct ExtensionEntry { const char* name; uint32_t version; };
static const ExtensionEntry kSupportedExtensions[] = {
    // Graphics bindings — D3D11 is native; others are stubs that return the
    // same adapter LUID. Apps that merely check extension presence at startup
    // (and use D3D11 anyway) will work. Apps that actually bind Vulkan/GL/D3D12
    // will get XR_ERROR_GRAPHICS_DEVICE_INVALID from xrCreateSession.
    { "XR_KHR_D3D11_enable",                         1 },
    { "XR_KHR_D3D12_enable",                         9 },
    { "XR_KHR_opengl_enable",                        10 },
    { "XR_KHR_vulkan_enable",                         8 },
    { "XR_KHR_vulkan_enable2",                        2 },
    // Depth layer — accept and ignore depth swapchains
    { "XR_KHR_composition_layer_depth",               6 },
    // Debug utils — stub
    { "XR_EXT_debug_utils",                           5 },
    // Win32 time conversion — QPC == XrTime in this runtime
    { "XR_KHR_win32_convert_performance_counter_time", 1 },
    // Visibility mask — stub: returns empty mesh
    { "XR_KHR_visibility_mask",                       2 },
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
        strncpy_s(props[i].extensionName, kSupportedExtensions[i].name,
                  XR_MAX_EXTENSION_NAME_SIZE - 1);
        props[i].extensionVersion = kSupportedExtensions[i].version;
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
                        kSupportedExtensions[j].name) == 0) {
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
// xrPollEvent — drain the runtime event queue
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrPollEvent(XrInstance /*instance*/, XrEventDataBuffer* eventData)
{
    xr3dv::Runtime* rt = xr3dv::GetRuntime();
    if (!rt) return XR_EVENT_UNAVAILABLE;

    std::lock_guard<std::mutex> lk(rt->eventMtx);
    if (rt->eventQueue.empty()) return XR_EVENT_UNAVAILABLE;

    *eventData = rt->eventQueue.front();
    rt->eventQueue.pop_front();

    // Log the delivered event for diagnostics
    if (eventData->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        const auto* ev = reinterpret_cast<const XrEventDataSessionStateChanged*>(eventData);
        LOG_VERBOSE("xrPollEvent: session state -> %d", (int)ev->state);
    }
    return XR_SUCCESS;
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
xrEnumerateEnvironmentBlendModes(
    XrInstance              /*instance*/,
    XrSystemId              /*systemId*/,
    XrViewConfigurationType viewConfigurationType,
    uint32_t                environmentBlendModeCapacityInput,
    uint32_t*               environmentBlendModeCountOutput,
    XrEnvironmentBlendMode* environmentBlendModes)
{
    // XR3DV presents to a physical display — opaque is the only blend mode.
    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;

    *environmentBlendModeCountOutput = 1;
    if (environmentBlendModeCapacityInput == 0)
        return XR_SUCCESS;
    if (environmentBlendModeCapacityInput < 1)
        return XR_ERROR_SIZE_INSUFFICIENT;

    environmentBlendModes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
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
// Helper: get the default GPU's LUID and feature level via a throwaway D3D11 device.
static bool GetDefaultAdapterLUID(LUID& luid, D3D_FEATURE_LEVEL& fl)
{
    Microsoft::WRL::ComPtr<ID3D11Device> dev;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                                    nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                    &dev, &fl, nullptr);
    if (FAILED(hr)) return false;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(dev.As(&dxgiDev))) return false;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(&adapter))) return false;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) return false;
    luid = desc.AdapterLuid;
    LOG_INFO("Default GPU: '%ls' LUID={%08X,%08X} fl=0x%04X",
             desc.Description, (unsigned)luid.HighPart, (unsigned)luid.LowPart, (unsigned)fl);
    return true;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetD3D11GraphicsRequirementsKHR(XrInstance /*instance*/,
                                   XrSystemId /*systemId*/,
                                   XrGraphicsRequirementsD3D11KHR* req)
{
    req->type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
    req->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    req->adapterLuid = {};
    D3D_FEATURE_LEVEL fl{}; LUID luid{};
    if (GetDefaultAdapterLUID(luid, fl)) {
        req->adapterLuid     = luid;
        req->minFeatureLevel = fl;
        LOG_INFO("xrGetD3D11GraphicsRequirementsKHR: LUID={%08X,%08X} fl=0x%04X",
                 (unsigned)luid.HighPart, (unsigned)luid.LowPart, (unsigned)fl);
    } else {
        LOG_ERROR("xrGetD3D11GraphicsRequirementsKHR: failed to query adapter");
    }
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// D3D12 graphics requirements stub (XR_KHR_D3D12_enable)
// We report the same adapter LUID so the app picks the right GPU.
// xrCreateSession with a D3D12 binding returns XR_ERROR_GRAPHICS_DEVICE_INVALID
// unless the app also provides a D3D11 binding (fallback chain).
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetD3D12GraphicsRequirementsKHR(XrInstance /*instance*/,
                                   XrSystemId /*systemId*/,
                                   XrGraphicsRequirementsD3D12KHR* req)
{
    req->type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;
    req->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    req->adapterLuid = {};
    D3D_FEATURE_LEVEL fl{}; LUID luid{};
    if (GetDefaultAdapterLUID(luid, fl))
        req->adapterLuid = luid;
    LOG_INFO("xrGetD3D12GraphicsRequirementsKHR: stub (LUID={%08X,%08X})",
             (unsigned)req->adapterLuid.HighPart, (unsigned)req->adapterLuid.LowPart);
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// OpenGL graphics requirements stub (XR_KHR_opengl_enable)
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetOpenGLGraphicsRequirementsKHR(XrInstance /*instance*/,
                                    XrSystemId /*systemId*/,
                                    XrGraphicsRequirementsOpenGLKHR* req)
{
    req->type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR;
    // Require OpenGL 4.0+ (matches most 3DV-capable NVIDIA hardware)
    req->minApiVersionSupported = XR_MAKE_VERSION(4, 0, 0);
    req->maxApiVersionSupported = XR_MAKE_VERSION(4, 6, 0);
    LOG_INFO("xrGetOpenGLGraphicsRequirementsKHR: stub");
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Vulkan graphics requirements stubs (XR_KHR_vulkan_enable / vulkan_enable2)
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetVulkanGraphicsRequirementsKHR(XrInstance /*instance*/,
                                    XrSystemId /*systemId*/,
                                    XrGraphicsRequirementsVulkanKHR* req)
{
    req->type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
    req->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    req->maxApiVersionSupported = XR_MAKE_VERSION(1, 3, 0);
    LOG_INFO("xrGetVulkanGraphicsRequirementsKHR: stub");
    return XR_SUCCESS;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetVulkanGraphicsRequirements2KHR(XrInstance instance,
                                     XrSystemId systemId,
                                     XrGraphicsRequirementsVulkanKHR* req)
{
    return xrGetVulkanGraphicsRequirementsKHR(instance, systemId, req);
}

// Vulkan instance/device extension query stubs — return empty lists
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetVulkanInstanceExtensionsKHR(XrInstance /*instance*/, XrSystemId /*systemId*/,
                                   uint32_t cap, uint32_t* count, char* buf)
{
    *count = 0;
    if (cap > 0 && buf) buf[0] = '\0';
    return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetVulkanDeviceExtensionsKHR(XrInstance /*instance*/, XrSystemId /*systemId*/,
                                 uint32_t cap, uint32_t* count, char* buf)
{
    *count = 0;
    if (cap > 0 && buf) buf[0] = '\0';
    return XR_SUCCESS;
}
// xrGetVulkanGraphicsDeviceKHR — we cannot return a VkPhysicalDevice without
// a Vulkan instance, so return XR_ERROR_FUNCTION_UNSUPPORTED. Apps that need
// this are genuinely Vulkan-native and cannot use XR3DV for presentation.
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetVulkanGraphicsDeviceKHR(XrInstance /*instance*/, XrSystemId /*systemId*/,
                               void* /*vkInstance*/, void** /*vkPhysicalDevice*/)
{
    LOG_ERROR("xrGetVulkanGraphicsDeviceKHR: Vulkan native rendering not supported by XR3DV");
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetVulkanGraphicsDevice2KHR(XrInstance instance, const void* /*getInfo*/,
                               void** vkPhysicalDevice)
{
    return xrGetVulkanGraphicsDeviceKHR(instance, 0, nullptr, vkPhysicalDevice);
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrCreateVulkanInstanceKHR(XrInstance /*instance*/, const void* /*createInfo*/,
                           void** /*vkInstance*/, int* /*vkResult*/)
{
    LOG_ERROR("xrCreateVulkanInstanceKHR: Vulkan native rendering not supported by XR3DV");
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrCreateVulkanDeviceKHR(XrInstance /*instance*/, const void* /*createInfo*/,
                         void** /*vkDevice*/, int* /*vkResult*/)
{
    LOG_ERROR("xrCreateVulkanDeviceKHR: Vulkan native rendering not supported by XR3DV");
    return XR_ERROR_FUNCTION_UNSUPPORTED;
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

    // Scan the next chain for a supported graphics binding
    const XrGraphicsBindingD3D11KHR* d3d11Binding = nullptr;
    const XrGraphicsBindingD3D12KHR* d3d12Binding = nullptr;
    const XrBaseInStructure* next =
        reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
    while (next) {
        if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
            d3d11Binding = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next);
        else if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR)
            d3d12Binding = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(next);
        next = next->next;
    }

    auto sess = std::make_unique<xr3dv::Session>(rt->cfg);
    XrResult res = XR_ERROR_GRAPHICS_DEVICE_INVALID;

    if (d3d11Binding) {
        res = sess->InitD3D11(d3d11Binding);
    } else if (d3d12Binding) {
        res = sess->InitD3D12(d3d12Binding->device, d3d12Binding->queue);
    } else {
        // Log what binding types were actually provided
        const XrBaseInStructure* n2 =
            reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
        while (n2) {
            LOG_ERROR("xrCreateSession: unsupported binding type %d "
                      "(XR3DV supports D3D11 natively, D3D12 via shared textures; "
                      "Vulkan/OpenGL not yet supported)", (int)n2->type);
            n2 = n2->next;
        }
        if (!createInfo->next)
            LOG_ERROR("xrCreateSession: no graphics binding in next chain");
    }
    if (res != XR_SUCCESS) return res;

    std::lock_guard<std::mutex> lk(rt->sessionMtx);
    uint64_t h = rt->nextSessionHandle++;
    rt->sessions[h] = std::move(sess);
    *session = reinterpret_cast<XrSession>(h);

    // Per OpenXR spec the runtime must deliver session state transitions via
    // xrPollEvent before the app can call xrBeginSession.
    // Queue: UNKNOWN → IDLE → READY so the app's event loop unblocks.
    XrTime now = xr3dv::FrameTimer::NowNs();
    XrSession xrSess = reinterpret_cast<XrSession>(h);
    rt->PushSessionStateEvent(xrSess, XR_SESSION_STATE_IDLE,  now);
    rt->PushSessionStateEvent(xrSess, XR_SESSION_STATE_READY, now);
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
    GET_SESSION(session);
    XrResult r = sess->Begin(info);
    if (r == XR_SUCCESS) {
        // Transition through SYNCHRONIZED → VISIBLE → FOCUSED so the app
        // enters its frame loop.  We skip SYNCHRONIZED/VISIBLE dwell time
        // since XR3DV has no display compositor arbitration.
        XrTime now = xr3dv::FrameTimer::NowNs();
        rt->PushSessionStateEvent(session, XR_SESSION_STATE_SYNCHRONIZED, now);
        rt->PushSessionStateEvent(session, XR_SESSION_STATE_VISIBLE,      now);
        rt->PushSessionStateEvent(session, XR_SESSION_STATE_FOCUSED,      now);
    }
    return r;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrEndSession(XrSession session)
{
    GET_SESSION(session);
    XrResult r = sess->End();
    if (r == XR_SUCCESS) {
        XrTime now = xr3dv::FrameTimer::NowNs();
        rt->PushSessionStateEvent(session, XR_SESSION_STATE_STOPPING, now);
        rt->PushSessionStateEvent(session, XR_SESSION_STATE_IDLE,     now);
    }
    return r;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrRequestExitSession(XrSession session)
{
    GET_SESSION(session);
    XrResult r = sess->RequestExit();
    if (r == XR_SUCCESS) {
        XrTime now = xr3dv::FrameTimer::NowNs();
        rt->PushSessionStateEvent(session, XR_SESSION_STATE_STOPPING, now);
        rt->PushSessionStateEvent(session, XR_SESSION_STATE_IDLE,     now);
        rt->PushSessionStateEvent(session, XR_SESSION_STATE_EXITING,  now);
    }
    return r;
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

    // The app fills images[0].type before calling; check it to return the
    // correct resource type. Default to D3D11 for unrecognised types.
    if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) {
        auto* d3d12Images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
        for (uint32_t i = 0; i < n; ++i) {
            d3d12Images[i].type    = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
            d3d12Images[i].next    = nullptr;
            d3d12Images[i].texture = sc->Images()[i].d3d12Tex.Get();
        }
    } else {
        auto* d3d11Images = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        for (uint32_t i = 0; i < n; ++i) {
            d3d11Images[i].type    = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
            d3d11Images[i].next    = nullptr;
            d3d11Images[i].texture = sc->Images()[i].tex.Get();
        }
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
// Extension stubs
// ---------------------------------------------------------------------------

// XR_EXT_debug_utils — accept messenger creation, do nothing with it
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrCreateDebugUtilsMessengerEXT(XrInstance /*instance*/,
    const XrDebugUtilsMessengerCreateInfoEXT* /*createInfo*/,
    XrDebugUtilsMessengerEXT* messenger)
{
    if (messenger) *messenger = reinterpret_cast<XrDebugUtilsMessengerEXT>(1);
    return XR_SUCCESS;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrDestroyDebugUtilsMessengerEXT(XrDebugUtilsMessengerEXT /*messenger*/)
{
    return XR_SUCCESS;
}

// XR_KHR_win32_convert_performance_counter_time
// XrTime == QPC value in 100ns units in this runtime, so conversion is 1:1.
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrConvertWin32PerformanceCounterToTimeKHR(XrInstance /*instance*/,
    const LARGE_INTEGER* performanceCounter, XrTime* time)
{
    if (!performanceCounter || !time) return XR_ERROR_VALIDATION_FAILURE;
    *time = static_cast<XrTime>(performanceCounter->QuadPart);
    return XR_SUCCESS;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrConvertTimeToWin32PerformanceCounterKHR(XrInstance /*instance*/,
    XrTime time, LARGE_INTEGER* performanceCounter)
{
    if (!performanceCounter) return XR_ERROR_VALIDATION_FAILURE;
    performanceCounter->QuadPart = static_cast<LONGLONG>(time);
    return XR_SUCCESS;
}

// XR_KHR_visibility_mask — return an empty mask (no occlusion zones for 3DV)
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetVisibilityMaskKHR(XrSession /*session*/,
    XrViewConfigurationType /*viewConfigurationType*/,
    uint32_t /*viewIndex*/,
    XrVisibilityMaskTypeKHR /*visibilityMaskType*/,
    XrVisibilityMaskKHR* visibilityMask)
{
    if (!visibilityMask) return XR_ERROR_VALIDATION_FAILURE;
    visibilityMask->vertexCountOutput  = 0;
    visibilityMask->indexCountOutput   = 0;
    return XR_SUCCESS;
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
    DISPATCH(xrEnumerateEnvironmentBlendModes)
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
    // XR_KHR_D3D12_enable (stub — reports LUID, session creation unsupported)
    DISPATCH(xrGetD3D12GraphicsRequirementsKHR)
    // XR_KHR_opengl_enable (stub)
    DISPATCH(xrGetOpenGLGraphicsRequirementsKHR)
    // XR_KHR_vulkan_enable (stub)
    DISPATCH(xrGetVulkanGraphicsRequirementsKHR)
    DISPATCH(xrGetVulkanInstanceExtensionsKHR)
    DISPATCH(xrGetVulkanDeviceExtensionsKHR)
    DISPATCH(xrGetVulkanGraphicsDeviceKHR)
    // XR_KHR_vulkan_enable2 (stub)
    DISPATCH(xrGetVulkanGraphicsRequirements2KHR)
    DISPATCH(xrCreateVulkanInstanceKHR)
    DISPATCH(xrCreateVulkanDeviceKHR)
    DISPATCH(xrGetVulkanGraphicsDevice2KHR)
    // XR_EXT_debug_utils
    DISPATCH(xrCreateDebugUtilsMessengerEXT)
    DISPATCH(xrDestroyDebugUtilsMessengerEXT)
    // XR_KHR_win32_convert_performance_counter_time
    DISPATCH(xrConvertWin32PerformanceCounterToTimeKHR)
    DISPATCH(xrConvertTimeToWin32PerformanceCounterKHR)
    // XR_KHR_visibility_mask
    DISPATCH(xrGetVisibilityMaskKHR)
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
