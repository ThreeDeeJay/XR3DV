//  XR3DV Diagnostic Tool
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  Standalone console utility that walks every layer of the XR3DV stack
//  and reports PASS / WARN / FAIL for each check.  Run this after
//  install.ps1 to confirm the runtime is correctly registered and functional.
//
//  Does NOT link against xr3dv.dll — it discovers and loads everything
//  dynamically so it can diagnose issues even when the loader itself fails.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <shlwapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#ifndef XR_USE_GRAPHICS_API_D3D11
#  define XR_USE_GRAPHICS_API_D3D11
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>

using Microsoft::WRL::ComPtr;

// ============================================================================
// Colour console output
// ============================================================================
static HANDLE g_con;

enum class C { Reset, Green, Yellow, Red, Cyan, White };
static void SetColor(C c) {
    WORD a = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE; // white
    switch (c) {
        case C::Green:  a = FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
        case C::Yellow: a = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
        case C::Red:    a = FOREGROUND_RED | FOREGROUND_INTENSITY; break;
        case C::Cyan:   a = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
        case C::White:  a = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
        default: break;
    }
    SetConsoleTextAttribute(g_con, a);
}

static void ResetColor() { SetColor(C::Reset); }

// ============================================================================
// Result tracking
// ============================================================================
struct Result {
    enum class Status { Pass, Warn, Fail, Info, Skip };
    Status      status;
    std::string label;
    std::string detail;
};

static std::vector<Result> g_results;
static int g_pass = 0, g_warn = 0, g_fail = 0;

static void Emit(Result::Status s, const char* label, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    g_results.push_back({s, label, buf});

    const char* tag;
    C col;
    switch (s) {
        case Result::Status::Pass: tag = "PASS"; col = C::Green;  ++g_pass; break;
        case Result::Status::Warn: tag = "WARN"; col = C::Yellow; ++g_warn; break;
        case Result::Status::Fail: tag = "FAIL"; col = C::Red;    ++g_fail; break;
        case Result::Status::Info: tag = "INFO"; col = C::Cyan;   break;
        default:                   tag = "SKIP"; col = C::White;  break;
    }

    SetColor(col);
    printf("[%s]", tag);
    ResetColor();
    printf(" %-42s %s\n", label, buf);
}

#define PASS(lbl, ...) Emit(Result::Status::Pass, lbl, __VA_ARGS__)
#define WARN(lbl, ...) Emit(Result::Status::Warn, lbl, __VA_ARGS__)
#define FAIL(lbl, ...) Emit(Result::Status::Fail, lbl, __VA_ARGS__)
#define INFO(lbl, ...) Emit(Result::Status::Info, lbl, __VA_ARGS__)
#define SKIP(lbl, ...) Emit(Result::Status::Skip, lbl, __VA_ARGS__)

// ============================================================================
// Helpers
// ============================================================================
static std::string RegReadString(HKEY root, const char* subkey, const char* value) {
    HKEY hk;
    if (RegOpenKeyExA(root, subkey, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return "";
    char buf[1024] = {};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    if (RegQueryValueExA(hk, value, nullptr, &type, (LPBYTE)buf, &sz) != ERROR_SUCCESS) {
        RegCloseKey(hk);
        return "";
    }
    RegCloseKey(hk);
    return std::string(buf);
}

static std::string ReadFileText(const std::string& path) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    DWORD sz = GetFileSize(h, nullptr);
    std::string s(sz, '\0');
    DWORD read = 0;
    ReadFile(h, s.data(), sz, &read, nullptr);
    CloseHandle(h);
    s.resize(read);
    return s;
}

// Very small JSON field extractor (no dependency on a JSON library)
static std::string JsonField(const std::string& json, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return "";
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        ++pos;
        size_t end = json.find('"', pos);
        return json.substr(pos, end - pos);
    }
    // bare value
    size_t end = json.find_first_of(",}\n", pos);
    std::string v = json.substr(pos, end - pos);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    return v;
}

// ============================================================================
// Section 1 — Registry
// ============================================================================
static std::string g_registeredJson;

static void CheckRegistry() {
    printf("\n");
    SetColor(C::White); printf("=== 1. Registry ===\n"); ResetColor();

    const char* subkey = "SOFTWARE\\Khronos\\OpenXR\\1";

    // HKCU (loader prefers this)
    std::string hkcu = RegReadString(HKEY_CURRENT_USER,  subkey, "ActiveRuntime");
    std::string hklm = RegReadString(HKEY_LOCAL_MACHINE, subkey, "ActiveRuntime");

    if (!hkcu.empty()) {
        PASS("HKCU ActiveRuntime", "%s", hkcu.c_str());
        g_registeredJson = hkcu;
    } else {
        WARN("HKCU ActiveRuntime", "Not set (per-user registration missing)");
    }

    if (!hklm.empty()) {
        PASS("HKLM ActiveRuntime", "%s", hklm.c_str());
        if (g_registeredJson.empty()) g_registeredJson = hklm;
    } else {
        WARN("HKLM ActiveRuntime", "Not set (system-wide registration missing)");
    }

    if (g_registeredJson.empty()) {
        FAIL("Any ActiveRuntime", "Neither HKCU nor HKLM have ActiveRuntime set");
        return;
    }

    // Does the registered value point at XR3DV?
    if (g_registeredJson.find("xr3dv") != std::string::npos ||
        g_registeredJson.find("XR3DV") != std::string::npos) {
        PASS("Points to XR3DV", "JSON path contains 'xr3dv'");
    } else {
        WARN("Points to XR3DV", "Registered runtime appears to be a DIFFERENT runtime: %s",
             g_registeredJson.c_str());
    }
}

// ============================================================================
// Section 2 — Manifest JSON
// ============================================================================
static std::string g_dllPath;

static void CheckManifest() {
    printf("\n");
    SetColor(C::White); printf("=== 2. Manifest JSON ===\n"); ResetColor();

    if (g_registeredJson.empty()) {
        SKIP("Manifest JSON", "No registry path to check");
        return;
    }

    if (!PathFileExistsA(g_registeredJson.c_str())) {
        FAIL("JSON file exists", "Not found: %s", g_registeredJson.c_str());
        return;
    }
    PASS("JSON file exists", "%s", g_registeredJson.c_str());

    std::string json = ReadFileText(g_registeredJson);
    if (json.empty()) {
        FAIL("JSON readable", "Could not read file");
        return;
    }
    PASS("JSON readable", "%zu bytes", json.size());

    // Required fields
    std::string ffv        = JsonField(json, "file_format_version");
    std::string apiVersion = JsonField(json, "api_version");
    std::string libPath    = JsonField(json, "library_path");
    std::string name       = JsonField(json, "name");

    if (ffv == "1.0.0") {
        PASS("file_format_version", "1.0.0");
    } else {
        FAIL("file_format_version", "Expected '1.0.0', got '%s'", ffv.c_str());
    }

    // api_version must be three-part e.g. "1.0.0" — "1.0" alone causes some loaders to reject it
    {
        int maj = 0, min = 0, patch = -1;
        sscanf_s(apiVersion.c_str(), "%d.%d.%d", &maj, &min, &patch);
        if (patch >= 0 && maj == 1) {
            PASS("api_version", "%s (three-part, correct)", apiVersion.c_str());
        } else {
            FAIL("api_version", "'%s' — must be three-part (e.g. 1.0.0); "
                 "some loaders reject two-part versions", apiVersion.c_str());
        }
    }

    if (!name.empty()) {
        PASS("runtime.name", "%s", name.c_str());
    } else {
        WARN("runtime.name", "Field missing");
    }

    if (libPath.empty()) {
        FAIL("library_path", "Field missing from JSON");
        return;
    }

    // Resolve relative path relative to the JSON's directory
    char resolved[MAX_PATH];
    if (PathIsRelativeA(libPath.c_str())) {
        char dir[MAX_PATH];
        strncpy_s(dir, g_registeredJson.c_str(), MAX_PATH - 1);
        PathRemoveFileSpecA(dir);
        PathCombineA(resolved, dir, libPath.c_str());
        INFO("library_path", "Relative '%s' -> '%s'", libPath.c_str(), resolved);
    } else {
        strncpy_s(resolved, libPath.c_str(), MAX_PATH - 1);
        INFO("library_path", "Absolute: %s", resolved);
    }

    g_dllPath = resolved;

    if (PathFileExistsA(resolved)) {
        PASS("DLL file exists", "%s", resolved);
    } else {
        FAIL("DLL file exists", "NOT FOUND: %s", resolved);
    }
}

// ============================================================================
// Section 3 — DLL load & export checks
// ============================================================================
static HMODULE  g_hDll    = nullptr;
typedef XrResult (XRAPI_PTR *PFN_xrNegotiate)(
    const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest*);

static PFN_xrNegotiate          g_pfnNegotiate   = nullptr;
static PFN_xrGetInstanceProcAddr g_pfnGetProcAddr = nullptr;

static void CheckDllExports() {
    printf("\n");
    SetColor(C::White); printf("=== 3. DLL Load & Exports ===\n"); ResetColor();

    if (g_dllPath.empty()) {
        SKIP("DLL load", "No DLL path resolved");
        return;
    }

    g_hDll = LoadLibraryA(g_dllPath.c_str());
    if (!g_hDll) {
        DWORD err = GetLastError();
        char msg[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err, 0, msg, sizeof(msg), nullptr);
        // Trim trailing newline
        for (int i = (int)strlen(msg) - 1; i >= 0 && (msg[i] == '\r' || msg[i] == '\n'); --i)
            msg[i] = '\0';
        FAIL("LoadLibrary", "Error %lu: %s", err, msg);
        return;
    }
    PASS("LoadLibrary", "xr3dv.dll loaded at 0x%p", (void*)g_hDll);

    // Mandatory export: xrNegotiateLoaderRuntimeInterface
    g_pfnNegotiate = (PFN_xrNegotiate)
        GetProcAddress(g_hDll, "xrNegotiateLoaderRuntimeInterface");
    if (g_pfnNegotiate) {
        PASS("Export: xrNegotiate...", "Found");
    } else {
        FAIL("Export: xrNegotiate...", "NOT exported — loader cannot attach");
        return;
    }

    // xrGetInstanceProcAddr (usually obtained via negotiate, but check export too)
    g_pfnGetProcAddr = (PFN_xrGetInstanceProcAddr)
        GetProcAddress(g_hDll, "xrGetInstanceProcAddr");
    if (g_pfnGetProcAddr) {
        PASS("Export: xrGetInstanceProcAddr", "Found");
    } else {
        WARN("Export: xrGetInstanceProcAddr", "Not directly exported (will be obtained via negotiate)");
    }

    // Check a few more mandatory OpenXR core exports
    const char* coreExports[] = {
        "xrCreateInstance", "xrDestroyInstance", "xrCreateSession",
        "xrWaitFrame", "xrBeginFrame", "xrEndFrame",
        "xrCreateSwapchain", "xrLocateViews",
    };
    bool allCore = true;
    for (auto* name : coreExports) {
        if (!GetProcAddress(g_hDll, name)) {
            FAIL("Export core function", "%s missing", name);
            allCore = false;
        }
    }
    if (allCore) PASS("Core function exports", "All %zu present",
                       sizeof(coreExports)/sizeof(coreExports[0]));
}

// ============================================================================
// Section 4 — Loader negotiation
// ============================================================================
static PFN_xrGetInstanceProcAddr g_negotiatedGetProcAddr = nullptr;

static void CheckNegotiation() {
    printf("\n");
    SetColor(C::White); printf("=== 4. Loader Negotiation ===\n"); ResetColor();

    if (!g_pfnNegotiate) {
        SKIP("Negotiation", "xrNegotiateLoaderRuntimeInterface not available");
        return;
    }

    XrNegotiateLoaderInfo loaderInfo{};
    loaderInfo.structType        = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    loaderInfo.structVersion     = XR_LOADER_INFO_STRUCT_VERSION;
    loaderInfo.structSize        = sizeof(loaderInfo);
    loaderInfo.minInterfaceVersion = 1;
    loaderInfo.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    loaderInfo.minApiVersion     = XR_MAKE_VERSION(1, 0, 0);
    loaderInfo.maxApiVersion     = XR_MAKE_VERSION(1, 0, 0xfff);

    XrNegotiateRuntimeRequest runtimeRequest{};
    runtimeRequest.structType    = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    runtimeRequest.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    runtimeRequest.structSize    = sizeof(runtimeRequest);

    XrResult r = g_pfnNegotiate(&loaderInfo, &runtimeRequest);
    if (r == XR_SUCCESS) {
        PASS("xrNegotiateLoader...", "XR_SUCCESS");
    } else {
        FAIL("xrNegotiateLoader...", "Failed: %d", (int)r);
        return;
    }

    INFO("  runtimeInterfaceVersion", "%u", runtimeRequest.runtimeInterfaceVersion);
    INFO("  runtimeApiVersion",       "0x%016llX", (unsigned long long)runtimeRequest.runtimeApiVersion);

    if (runtimeRequest.getInstanceProcAddr) {
        PASS("getInstanceProcAddr returned", "0x%p", (void*)runtimeRequest.getInstanceProcAddr);
        g_negotiatedGetProcAddr = runtimeRequest.getInstanceProcAddr;
    } else {
        FAIL("getInstanceProcAddr returned", "NULL — loader cannot dispatch OpenXR calls");
    }
}

// ============================================================================
// Section 5 — OpenXR API via the loader (high-level path)
// ============================================================================
// We call through the SYSTEM loader (openxr_loader.dll) if present, not
// directly through our DLL, to test the full real-world path.
// ============================================================================

typedef XrResult(XRAPI_PTR* PFN_xrCreateInstance)(
    const XrInstanceCreateInfo*, XrInstance*);

static XrInstance g_instance = XR_NULL_HANDLE;
static PFN_xrGetInstanceProcAddr g_loaderGIPA = nullptr;

template<typename PFN>
static bool GetXrProc(const char* name, PFN& out) {
    if (!g_loaderGIPA) return false;
    XrResult r = g_loaderGIPA(g_instance, name, (PFN_xrVoidFunction*)&out);
    return r == XR_SUCCESS && out != nullptr;
}

static void CheckOpenXRLoader() {
    printf("\n");
    SetColor(C::White); printf("=== 5. OpenXR Loader (system path) ===\n"); ResetColor();

    // Try to load the OpenXR loader DLL — check several locations
    HMODULE hLoader = LoadLibraryA("openxr_loader.dll");
    if (!hLoader) hLoader = LoadLibraryA("openxr_loader-1_0.dll");

    // Check Khronos SDK install path from registry
    if (!hLoader) {
        std::string sdkPath = RegReadString(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Khronos\\OpenXR\\1", "");   // default value sometimes has SDK path
        if (sdkPath.empty()) {
            // Some SDK installers write the bin dir under a separate key
            sdkPath = RegReadString(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Khronos\\OpenXR\\1", "SDKBin");
        }
        if (!sdkPath.empty()) {
            std::string loaderPath = sdkPath + "\\openxr_loader.dll";
            hLoader = LoadLibraryA(loaderPath.c_str());
            if (hLoader) INFO("openxr_loader.dll", "Found via Khronos SDK registry");
        }
    }

    // Last resort: look next to our own exe (some apps ship the loader)
    if (!hLoader) {
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        PathRemoveFileSpecA(exePath);
        std::string localLoader = std::string(exePath) + "\\openxr_loader.dll";
        hLoader = LoadLibraryA(localLoader.c_str());
        if (hLoader) INFO("openxr_loader.dll", "Found next to xr3dv_diag.exe");
    }
    if (!hLoader) {
        WARN("openxr_loader.dll", "Not found in PATH, registry, or next to exe. "
             "Install the Khronos OpenXR SDK or add the loader to PATH. "
             "Testing via xr3dv.dll directly instead.");
        g_loaderGIPA = g_negotiatedGetProcAddr;
        if (!g_loaderGIPA) {
            SKIP("Loader path", "No getInstanceProcAddr available");
            return;
        }
        INFO("Loader path", "Using xr3dv.dll directly (no system loader)");
    } else {
        PASS("openxr_loader.dll", "Loaded at 0x%p", (void*)hLoader);
        g_loaderGIPA = (PFN_xrGetInstanceProcAddr)
            GetProcAddress(hLoader, "xrGetInstanceProcAddr");
        if (g_loaderGIPA) {
            PASS("Loader xrGetInstanceProcAddr", "Found");
        } else {
            FAIL("Loader xrGetInstanceProcAddr", "Not exported from loader");
            return;
        }
    }

    // ---- xrEnumerateInstanceExtensionProperties -------------------------
    {
        PFN_xrEnumerateInstanceExtensionProperties pfnEnum = nullptr;
        GetXrProc("xrEnumerateInstanceExtensionProperties", pfnEnum);
        if (!pfnEnum) {
            FAIL("xrEnumInstanceExtProps", "Proc not found");
        } else {
            uint32_t count = 0;
            XrResult r = pfnEnum(nullptr, 0, &count, nullptr);
            if (r == XR_SUCCESS) {
                std::vector<XrExtensionProperties> exts(count,
                    {XR_TYPE_EXTENSION_PROPERTIES});
                pfnEnum(nullptr, count, &count, exts.data());
                PASS("xrEnumInstanceExtProps", "%u extensions available", count);
                for (auto& e : exts) {
                    INFO("  Extension", "%s (v%u)", e.extensionName, e.extensionVersion);
                }
                // Check for required and important extensions
                bool hasD3D11 = false, hasDepth = false;
                for (auto& e : exts) {
                    if (strcmp(e.extensionName, "XR_KHR_D3D11_enable") == 0)            hasD3D11 = true;
                    if (strcmp(e.extensionName, "XR_KHR_composition_layer_depth") == 0) hasDepth = true;
                }
                if (hasD3D11) PASS("XR_KHR_D3D11_enable present", "Required extension found");
                else          FAIL("XR_KHR_D3D11_enable present", "MISSING -- XR3DV requires this");
                if (hasDepth) PASS("XR_KHR_composition_layer_depth", "Present (depth swapchains supported)");
                else          WARN("XR_KHR_composition_layer_depth", "Missing -- hello_xr will skip depth submission");
            } else {
                FAIL("xrEnumInstanceExtProps", "Returned %d", (int)r);
            }
        }
    }

    // ---- xrCreateInstance -----------------------------------------------
    {
        const char* exts[] = {"XR_KHR_D3D11_enable"};
        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
        ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        strncpy_s(ci.applicationInfo.applicationName, "xr3dv_diag", XR_MAX_APPLICATION_NAME_SIZE - 1);
        strncpy_s(ci.applicationInfo.engineName,      "xr3dv_diag", XR_MAX_ENGINE_NAME_SIZE - 1);
        ci.enabledExtensionCount = 1;
        ci.enabledExtensionNames = exts;

        PFN_xrCreateInstance pfnCreate = nullptr;
        GetXrProc("xrCreateInstance", pfnCreate);
        if (!pfnCreate) {
            FAIL("xrCreateInstance proc", "Not found");
            return;
        }

        XrResult r = pfnCreate(&ci, &g_instance);
        if (r == XR_SUCCESS) {
            PASS("xrCreateInstance", "XrInstance = 0x%p", (void*)g_instance);
        } else {
            FAIL("xrCreateInstance", "Failed: %d", (int)r);
            return;
        }
    }

    // Re-query GIPA bound to instance
    if (hLoader) {
        g_loaderGIPA = (PFN_xrGetInstanceProcAddr)
            GetProcAddress(hLoader, "xrGetInstanceProcAddr");
    }

    // ---- xrGetInstanceProperties ----------------------------------------
    {
        PFN_xrGetInstanceProperties pfn = nullptr;
        GetXrProc("xrGetInstanceProperties", pfn);
        if (pfn) {
            XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
            if (pfn(g_instance, &props) == XR_SUCCESS) {
                PASS("xrGetInstanceProperties", "Runtime: '%s' v%u.%u.%u",
                     props.runtimeName,
                     (unsigned)XR_VERSION_MAJOR(props.runtimeVersion),
                     (unsigned)XR_VERSION_MINOR(props.runtimeVersion),
                     (unsigned)XR_VERSION_PATCH(props.runtimeVersion));
            } else {
                WARN("xrGetInstanceProperties", "Call failed");
            }
        }
    }

    // ---- xrGetSystem ----------------------------------------------------
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    {
        PFN_xrGetSystem pfn = nullptr;
        GetXrProc("xrGetSystem", pfn);
        if (pfn) {
            XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
            sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            XrResult r = pfn(g_instance, &sgi, &systemId);
            if (r == XR_SUCCESS) {
                PASS("xrGetSystem", "SystemId = %llu", (unsigned long long)systemId);
            } else {
                FAIL("xrGetSystem", "Failed: %d", (int)r);
            }
        }
    }

    // ---- xrGetSystemProperties ------------------------------------------
    if (systemId != XR_NULL_SYSTEM_ID) {
        PFN_xrGetSystemProperties pfn = nullptr;
        GetXrProc("xrGetSystemProperties", pfn);
        if (pfn) {
            XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
            if (pfn(g_instance, systemId, &sp) == XR_SUCCESS) {
                PASS("xrGetSystemProperties", "System: '%s'", sp.systemName);
                INFO("  Max swapchain",  "%ux%u",
                     sp.graphicsProperties.maxSwapchainImageWidth,
                     sp.graphicsProperties.maxSwapchainImageHeight);
                INFO("  Orientation tracking", "%s",
                     sp.trackingProperties.orientationTracking ? "yes" : "no");
                INFO("  Position tracking",    "%s",
                     sp.trackingProperties.positionTracking ? "yes" : "no");
            }
        }
    }

    // ---- xrEnumerateViewConfigurationViews ------------------------------
    if (systemId != XR_NULL_SYSTEM_ID) {
        PFN_xrEnumerateViewConfigurationViews pfn = nullptr;
        GetXrProc("xrEnumerateViewConfigurationViews", pfn);
        if (pfn) {
            uint32_t cnt = 0;
            XrResult r = pfn(g_instance, systemId,
                              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &cnt, nullptr);
            if (r == XR_SUCCESS && cnt == 2) {
                std::vector<XrViewConfigurationView> vcv(cnt, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
                pfn(g_instance, systemId,
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, cnt, &cnt, vcv.data());
                PASS("View config (stereo)", "%u views", cnt);
                for (uint32_t i = 0; i < cnt; ++i) {
                    char eyeLabel[32];
                    snprintf(eyeLabel, sizeof(eyeLabel), "  Eye [%u]", i);
                    INFO(eyeLabel, "%ux%u recommended, %ux%u max",
                         vcv[i].recommendedImageRectWidth,
                         vcv[i].recommendedImageRectHeight,
                         vcv[i].maxImageRectWidth,
                         vcv[i].maxImageRectHeight);
                }
            } else {
                FAIL("View config (stereo)", "Expected 2 views, got %u (r=%d)", cnt, (int)r);
            }
        }
    }
    // ---- xrEnumerateEnvironmentBlendModes -------------------------------
    if (systemId != XR_NULL_SYSTEM_ID) {
        PFN_xrEnumerateEnvironmentBlendModes pfn = nullptr;
        GetXrProc("xrEnumerateEnvironmentBlendModes", pfn);
        if (!pfn) {
            FAIL("xrEnumEnvBlendModes", "Proc not found -- hello_xr will abort here");
        } else {
            uint32_t cnt = 0;
            XrResult r = pfn(g_instance, systemId,
                              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &cnt, nullptr);
            if (r == XR_SUCCESS && cnt > 0) {
                std::vector<XrEnvironmentBlendMode> modes(cnt);
                pfn(g_instance, systemId,
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, cnt, &cnt, modes.data());
                PASS("xrEnumEnvBlendModes", "%u mode(s)", cnt);
                for (uint32_t i = 0; i < cnt; ++i) {
                    const char* name =
                        (modes[i] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE)      ? "OPAQUE" :
                        (modes[i] == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE)    ? "ADDITIVE" :
                        (modes[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) ? "ALPHA_BLEND" :
                                                                               "UNKNOWN";
                    INFO("  Blend mode", "%s (%d)", name, (int)modes[i]);
                }
            } else {
                FAIL("xrEnumEnvBlendModes", "Returned r=%d cnt=%u", (int)r, cnt);
            }
        }
    }

} // end CheckOpenXRLoader

// ============================================================================
// Section 6 — D3D11 device + session creation
// ============================================================================
static XrSession g_session = XR_NULL_HANDLE;

static void CheckSessionCreation() {
    printf("\n");
    SetColor(C::White); printf("=== 6. D3D11 Session Creation ===\n"); ResetColor();

    if (g_instance == XR_NULL_HANDLE) {
        SKIP("Session creation", "No XrInstance");
        return;
    }

    // Create D3D11 device
    ComPtr<ID3D11Device>        d3dDev;
    ComPtr<ID3D11DeviceContext> d3dCtx;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &d3dDev, &fl, &d3dCtx);

    if (FAILED(hr)) {
        FAIL("D3D11CreateDevice", "0x%08X", hr);
        return;
    }
    PASS("D3D11CreateDevice", "Feature level 0x%04X", (unsigned)fl);

    // Check D3D11 graphics requirements first
    {
        PFN_xrGetD3D11GraphicsRequirementsKHR pfn = nullptr;
        GetXrProc("xrGetD3D11GraphicsRequirementsKHR", pfn);
        if (pfn) {
            XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
            XrSystemId sid = XR_NULL_SYSTEM_ID;
            // Get system id again
            PFN_xrGetSystem pfnSys = nullptr;
            GetXrProc("xrGetSystem", pfnSys);
            if (pfnSys) {
                XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
                sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
                pfnSys(g_instance, &sgi, &sid);
            }
            XrResult r = pfn(g_instance, sid, &req);
            if (r == XR_SUCCESS) {
                PASS("xrGetD3D11GraphicsReqs",
                     "MinFeatureLevel=0x%04X  AdapterLUID={%08X,%08X}",
                     (unsigned)req.minFeatureLevel,
                     (unsigned)req.adapterLuid.HighPart,
                     (unsigned)req.adapterLuid.LowPart);
                // Warn if LUID is still all-zeros (adapter enumeration failed in runtime)
                if (req.adapterLuid.HighPart == 0 && req.adapterLuid.LowPart == 0) {
                    WARN("AdapterLUID non-zero", "LUID is {0,0} -- apps that match by LUID will fail");
                } else {
                    PASS("AdapterLUID non-zero", "OK");
                }
                if (fl >= req.minFeatureLevel) {
                    PASS("Feature level sufficient", "0x%04X >= 0x%04X",
                         (unsigned)fl, (unsigned)req.minFeatureLevel);
                } else {
                    FAIL("Feature level sufficient", "Device 0x%04X < required 0x%04X",
                         (unsigned)fl, (unsigned)req.minFeatureLevel);
                }
            } else {
                WARN("xrGetD3D11GraphicsReqs", "Failed: %d", (int)r);
            }
        }
    }

    // Create session
    PFN_xrCreateSession pfnCS = nullptr;
    GetXrProc("xrCreateSession", pfnCS);
    if (!pfnCS) { FAIL("xrCreateSession proc", "Not found"); return; }

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = d3dDev.Get();

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    // Get system id
    PFN_xrGetSystem pfnSys = nullptr;
    GetXrProc("xrGetSystem", pfnSys);
    if (pfnSys) {
        XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        pfnSys(g_instance, &sgi, &sci.systemId);
    }

    XrResult r = pfnCS(g_instance, &sci, &g_session);
    if (r == XR_SUCCESS) {
        PASS("xrCreateSession", "XrSession = 0x%p", (void*)g_session);
    } else {
        FAIL("xrCreateSession", "Failed: %d — check log at xr3dv.log", (int)r);
        return;
    }

    // xrBeginSession
    PFN_xrBeginSession pfnBS = nullptr;
    GetXrProc("xrBeginSession", pfnBS);
    if (pfnBS) {
        XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
        sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        r = pfnBS(g_session, &sbi);
        if (r == XR_SUCCESS) PASS("xrBeginSession", "OK");
        else                  FAIL("xrBeginSession", "Failed: %d", (int)r);
    }
}

// ============================================================================
// Section 7 — Frame loop smoke test (3 frames)
// ============================================================================
static void CheckFrameLoop() {
    printf("\n");
    SetColor(C::White); printf("=== 7. Frame Loop (3 frames) ===\n"); ResetColor();

    if (g_session == XR_NULL_HANDLE) {
        SKIP("Frame loop", "No XrSession");
        return;
    }

    PFN_xrWaitFrame  pfnWF  = nullptr;
    PFN_xrBeginFrame pfnBF  = nullptr;
    PFN_xrEndFrame   pfnEF  = nullptr;
    PFN_xrEnumerateSwapchainFormats pfnESF = nullptr;
    PFN_xrCreateSwapchain           pfnCSC = nullptr;

    GetXrProc("xrWaitFrame",  pfnWF);
    GetXrProc("xrBeginFrame", pfnBF);
    GetXrProc("xrEndFrame",   pfnEF);
    GetXrProc("xrEnumerateSwapchainFormats", pfnESF);
    GetXrProc("xrCreateSwapchain",           pfnCSC);

    if (!pfnWF || !pfnBF || !pfnEF) {
        FAIL("Frame procs", "WaitFrame/BeginFrame/EndFrame not all found");
        return;
    }

    // Create a minimal swapchain so EndFrame has something to submit
    XrSwapchain sc = XR_NULL_HANDLE;
    if (pfnESF && pfnCSC) {
        uint32_t fmtCnt = 0;
        pfnESF(g_session, 0, &fmtCnt, nullptr);
        std::vector<int64_t> fmts(fmtCnt);
        pfnESF(g_session, fmtCnt, &fmtCnt, fmts.data());

        if (!fmts.empty()) {
            XrSwapchainCreateInfo swci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            swci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            swci.format      = fmts[0];
            swci.sampleCount = 1;
            swci.width       = 1920;
            swci.height      = 1080;
            swci.faceCount   = 1;
            swci.arraySize   = 1;
            swci.mipCount    = 1;
            pfnCSC(g_session, &swci, &sc);
        }
    }

    bool frameOk = true;
    for (int f = 0; f < 3; ++f) {
        XrFrameWaitInfo  fwi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState     fst{XR_TYPE_FRAME_STATE};
        XrResult r = pfnWF(g_session, &fwi, &fst);
        if (r != XR_SUCCESS) {
            FAIL("WaitFrame", "Frame %d: %d", f, (int)r); frameOk = false; break;
        }

        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        r = pfnBF(g_session, &fbi);
        if (r != XR_SUCCESS) {
            FAIL("BeginFrame", "Frame %d: %d", f, (int)r); frameOk = false; break;
        }

        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime               = fst.predictedDisplayTime;
        fei.environmentBlendMode      = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount                = 0; // shouldRender might be false, submit nothing
        r = pfnEF(g_session, &fei);
        if (r != XR_SUCCESS) {
            FAIL("EndFrame", "Frame %d: %d", f, (int)r); frameOk = false; break;
        }
    }
    if (frameOk) {
        PASS("3-frame loop", "WaitFrame/BeginFrame/EndFrame all returned XR_SUCCESS");
    }
}

// ============================================================================
// Section 8 — NVAPI / 3D Vision availability
// ============================================================================

// NVAPI QueryInterface IDs we need (from nvapi.h / public NVAPI docs).
// These are stable across driver versions.
static const uint32_t kNvAPI_Initialize_ID          = 0x0150E828u;
static const uint32_t kNvAPI_Stereo_IsEnabled_ID     = 0x348FF8E1u;
static const uint32_t kNvAPI_GetErrorMessage_ID      = 0x6C2D048Cu;

typedef void*   (__cdecl* PFN_NvAPI_QueryInterface)(uint32_t id);
typedef int     (__cdecl* PFN_NvAPI_Initialize)();
typedef int     (__cdecl* PFN_NvAPI_Stereo_IsEnabled)(uint8_t* pIsStereoEnabled);

/// Try to call NvAPI_Stereo_IsEnabled directly via QueryInterface.
/// Returns: 1=enabled, 0=disabled, -1=could not determine
static int TryNvapiStereoIsEnabled(HMODULE hNvapi) {
    auto pfnQI = (PFN_NvAPI_QueryInterface)GetProcAddress(hNvapi, "nvapi_QueryInterface");
    if (!pfnQI) return -1;

    auto pfnInit = (PFN_NvAPI_Initialize)pfnQI(kNvAPI_Initialize_ID);
    if (!pfnInit) return -1;
    if (pfnInit() != 0 /*NVAPI_OK*/) return -1;  // 0 = NVAPI_OK

    auto pfnStereoIsEnabled = (PFN_NvAPI_Stereo_IsEnabled)pfnQI(kNvAPI_Stereo_IsEnabled_ID);
    if (!pfnStereoIsEnabled) return -1;

    uint8_t enabled = 0;
    int r = pfnStereoIsEnabled(&enabled);
    if (r != 0) return -1;
    return static_cast<int>(enabled);
}

/// Try running NvapiHelper.exe and reading result.txt.
/// Path: <exeDir>/Tools/NvapiHelper/NvapiHelper.exe
/// Produces: result.txt containing "True" or "False"
static int TryNvapiHelper(const std::string& exeDir) {
    std::string helperPath = exeDir + "\\Tools\\NvapiHelper\\NvapiHelper.exe";
    if (!PathFileExistsA(helperPath.c_str())) return -1;

    std::string resultPath = exeDir + "\\Tools\\NvapiHelper\\result.txt";
    // Delete stale result
    DeleteFileA(resultPath.c_str());

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::string cmdLine = "\"" + helperPath + "\" isStereo3dEnabled";
    std::vector<char> cmd(cmdLine.begin(), cmdLine.end());
    cmd.push_back('\0');

    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 5000 /*ms*/);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::string result = ReadFileText(resultPath);
    // Trim whitespace
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n' ||
                                result.back() == ' '))
        result.pop_back();

    if (result == "True")  return 1;
    if (result == "False") return 0;
    return -1;
}

static void CheckNvapi() {
    printf("\n");
    SetColor(C::White); printf("=== 8. NVAPI / 3D Vision ===\n"); ResetColor();

    // Determine our own exe directory for NvapiHelper search
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecA(exePath);
    std::string exeDir = exePath;

    // ------------------------------------------------------------------
    // 8a. Load nvapi64.dll
    // ------------------------------------------------------------------
    HMODULE hNvapi = LoadLibraryA("nvapi64.dll");
    if (!hNvapi) {
        char sysDir[MAX_PATH];
        GetSystemDirectoryA(sysDir, MAX_PATH);
        std::string p = std::string(sysDir) + "\\nvapi64.dll";
        hNvapi = LoadLibraryA(p.c_str());
    }
    if (!hNvapi) {
        FAIL("nvapi64.dll", "Not found — is an NVIDIA driver installed?");
        return;
    }
    PASS("nvapi64.dll", "Loaded");

    // ------------------------------------------------------------------
    // 8b. Determine 3D Vision stereo-enabled status.
    //     We try three methods in order of reliability:
    //       1. NvAPI_Stereo_IsEnabled via QueryInterface (most reliable)
    //       2. NvapiHelper.exe helper tool (if present in Tools/)
    //       3. Registry heuristics (WOW6432Node path used by modern drivers)
    // ------------------------------------------------------------------
    int stereoEnabled = -1;  // -1 = unknown
    const char* stereoMethod = nullptr;

    // Method 1 — direct NVAPI call
    stereoEnabled = TryNvapiStereoIsEnabled(hNvapi);
    if (stereoEnabled >= 0) {
        stereoMethod = "NvAPI_Stereo_IsEnabled()";
        INFO("3DV detection method", "Direct NVAPI call");
    }

    // Method 2 — NvapiHelper.exe
    if (stereoEnabled < 0) {
        stereoEnabled = TryNvapiHelper(exeDir);
        if (stereoEnabled >= 0) {
            stereoMethod = "NvapiHelper.exe";
            INFO("3DV detection method", "NvapiHelper.exe");
        }
    }

    // Method 3 — Registry (try multiple known paths)
    if (stereoEnabled < 0) {
        // Modern 64-bit drivers write to WOW6432Node
        const char* regPaths[] = {
            "SOFTWARE\\WOW6432Node\\NVIDIA Corporation\\Global\\Stereo3D",
            "SOFTWARE\\NVIDIA Corporation\\Global\\Stereo3D",
        };
        for (auto* path : regPaths) {
            std::string val = RegReadString(HKEY_LOCAL_MACHINE, path, "StereoEnable");
            if (!val.empty()) {
                stereoEnabled = (val == "1") ? 1 : 0;
                stereoMethod  = path;
                INFO("3DV detection method", "Registry: HKLM\\%s", path);
                break;
            }
        }
    }

    // Report the result
    if (stereoEnabled > 0) {
        PASS("3D Vision stereo enabled", "Yes (via %s)", stereoMethod ? stereoMethod : "?");
    } else if (stereoEnabled == 0) {
        WARN("3D Vision stereo enabled",
             "No (via %s) — enable in NVIDIA Control Panel: "
             "Manage 3D Settings > Stereo - Enable > On",
             stereoMethod ? stereoMethod : "?");
    } else {
        // Could not determine — give actionable advice without a hard FAIL
        WARN("3D Vision stereo enabled",
             "Could not determine state. "
             "Place NvapiHelper.exe at %s\\Tools\\NvapiHelper\\NvapiHelper.exe "
             "for a definitive check, or verify manually in NVIDIA Control Panel.",
             exeDir.c_str());
        INFO("Registry paths checked",
             "HKLM\\SOFTWARE\\WOW6432Node\\NVIDIA Corporation\\Global\\Stereo3D");
    }

    // ------------------------------------------------------------------
    // 8c. Display mode — 3D Vision requires >= 100 Hz (120 Hz ideal)
    // ------------------------------------------------------------------
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
        INFO("Primary display", "%ux%u @ %u Hz",
             dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
        if (dm.dmDisplayFrequency >= 100) {
            PASS("Display refresh rate", "%u Hz (>=100 Hz, OK for 3D Vision)",
                 dm.dmDisplayFrequency);
        } else {
            WARN("Display refresh rate",
                 "%u Hz -- 3D Vision requires >=100 Hz (ideally 120 Hz). "
                 "Change in Display Settings > Advanced display > Refresh rate.",
                 dm.dmDisplayFrequency);
        }
    }

    // ------------------------------------------------------------------
    // 8d. Confirm nvapi64.dll exports nvapi_QueryInterface (sanity)
    // ------------------------------------------------------------------
    if (GetProcAddress(hNvapi, "nvapi_QueryInterface")) {
        PASS("nvapi_QueryInterface export", "Present (expected)");
    } else {
        WARN("nvapi_QueryInterface export", "Missing -- unusual, driver may be incomplete");
    }

    FreeLibrary(hNvapi);
}


// ============================================================================
// Section 9 — Cleanup + Summary
// ============================================================================
static void Cleanup() {
    if (g_session != XR_NULL_HANDLE) {
        PFN_xrDestroySession pfn = nullptr;
        GetXrProc("xrDestroySession", pfn);
        if (pfn) pfn(g_session);
    }
    if (g_instance != XR_NULL_HANDLE) {
        PFN_xrDestroyInstance pfn = nullptr;
        GetXrProc("xrDestroyInstance", pfn);
        if (pfn) pfn(g_instance);
    }
    if (g_hDll) FreeLibrary(g_hDll);
}

static void PrintSummary() {
    printf("\n");
    SetColor(C::White);
    printf("====================================================\n");
    printf("  XR3DV Diagnostic Summary\n");
    printf("====================================================\n");
    ResetColor();

    printf("  ");
    SetColor(C::Green);  printf("PASS: %d  ", g_pass);
    SetColor(C::Yellow); printf("WARN: %d  ", g_warn);
    SetColor(C::Red);    printf("FAIL: %d\n", g_fail);
    ResetColor();

    if (g_fail == 0 && g_warn == 0) {
        SetColor(C::Green);
        printf("\n  All checks passed. XR3DV is ready.\n");
    } else if (g_fail == 0) {
        SetColor(C::Yellow);
        printf("\n  No failures, but %d warning(s) above need attention.\n", g_warn);
    } else {
        SetColor(C::Red);
        printf("\n  %d failure(s) detected. See FAIL lines above.\n", g_fail);
        printf("\n  Common fixes:\n");
        printf("    1. Re-run install.ps1 (it patches the JSON with the absolute DLL path)\n");
        printf("    2. Enable 3D Vision in NVIDIA Control Panel\n");
        printf("    3. Set your monitor to 120 Hz\n");
        printf("    4. Check xr3dv.log next to the DLL for runtime errors\n");
    }
    ResetColor();
    printf("\n");
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char** argv) {
    g_con = GetStdHandle(STD_OUTPUT_HANDLE);

    SetColor(C::White);
    printf("\n  XR3DV Diagnostic Tool\n");
    printf("  OpenXR runtime for NVIDIA 3D Vision\n");
    printf("  ====================================\n\n");
    ResetColor();

    // Parse args
    bool fastExit = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-pause") == 0) fastExit = true;
    }

    CheckRegistry();
    CheckManifest();
    CheckDllExports();
    CheckNegotiation();
    CheckOpenXRLoader();
    CheckSessionCreation();
    CheckFrameLoop();
    CheckNvapi();
    Cleanup();
    PrintSummary();

    if (!fastExit) {
        printf("Press Enter to exit...");
        getchar();
    }

    return (g_fail > 0) ? 1 : 0;
}
