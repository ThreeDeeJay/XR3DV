//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// D3D11 headers MUST come before openxr_platform.h, which gates the
// XrGraphicsBindingD3D11KHR family on XR_USE_GRAPHICS_API_D3D11.
#ifndef XR_USE_GRAPHICS_API_D3D11
#  define XR_USE_GRAPHICS_API_D3D11
#endif
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <thread>

#include "swapchain.h"
#include "nvapi_stereo.h"
#include "timing.h"
#include "config.h"

namespace xr3dv {

/// Represents a single XrSession (one application connection).
class Session {
public:
    explicit Session(Config& cfg);
    ~Session();

    // Non-copyable
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    /// Initialise from XR_KHR_D3D11_enable binding info.
    XrResult InitD3D11(const XrGraphicsBindingD3D11KHR* binding);

    // ---------- Lifecycle -------------------------------------------------
    XrResult Begin(const XrSessionBeginInfo* bi);
    XrResult End();
    XrResult RequestExit();

    // ---------- Frame loop ------------------------------------------------
    XrResult WaitFrame(const XrFrameWaitInfo* info, XrFrameState* state);
    XrResult BeginFrame(const XrFrameBeginInfo* info);
    XrResult EndFrame(const XrFrameEndInfo* info);

    // ---------- Swapchains ------------------------------------------------
    XrResult CreateSwapchain(const XrSwapchainCreateInfo* ci,
                             XrSwapchain*                 out);
    XrResult DestroySwapchain(XrSwapchain sc);
    XrResult EnumerateSwapchainFormats(uint32_t cap, uint32_t* cnt,
                                       int64_t* fmts);

    Swapchain* GetSwapchain(XrSwapchain handle);

    // ---------- Views -----------------------------------------------------
    XrResult LocateViews(const XrViewLocateInfo* info,
                         XrViewState*             viewState,
                         uint32_t                 viewCapacity,
                         uint32_t*                viewCount,
                         XrView*                  views);

    // ---------- Action sets -----------------------------------------------
    void AttachActionSets() {} // no-op

    // ---------- State  ----------------------------------------------------
    XrSessionState State() const { return m_state.load(); }
    bool IsRunning() const;

private:
    void PollConfigThread();
    bool PresentFrame(const XrFrameEndInfo* info);

    Config&                      m_cfg;
    std::atomic<XrSessionState>  m_state{XR_SESSION_STATE_IDLE};

    // D3D11 device provided by the application
    Microsoft::WRL::ComPtr<ID3D11Device>        m_d3d11Dev;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3d11Ctx;

    // NVAPI stereo presenter
    std::unique_ptr<NvapiStereoPresenter>        m_stereo;

    // Frame timing
    FrameTimer   m_timer;
    std::mutex   m_frameMtx;
    bool         m_frameBegun = false;
    int64_t      m_predictedDisplayTime = 0;

    // Swapchain registry
    std::mutex                                        m_scMtx;
    std::unordered_map<uint64_t, std::unique_ptr<Swapchain>> m_swapchains;
    uint64_t                                          m_nextScHandle = 1;

    // Background config-poll thread
    std::thread  m_pollThread;
    std::atomic<bool> m_pollStop{false};

    // Eye identification stored during EndFrame
    XrSwapchain  m_leftSwapchain  = XR_NULL_HANDLE;
    XrSwapchain  m_rightSwapchain = XR_NULL_HANDLE;
};

} // namespace xr3dv
