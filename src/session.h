//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
// All D3D11 / OpenXR platform headers are pulled in via pch.h which every
// .cpp includes first.  Headers only need their own forward-declarations or
// lightweight includes.
#include <openxr/openxr.h>   // core XR types (no platform structs needed here)
#include <d3d11.h>           // ID3D11Device etc. for function signatures
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

enum class GraphicsApi { D3D11, D3D12 };

/// Represents a single XrSession (one application connection).
class Session {
public:
    explicit Session(Config& cfg);
    ~Session();

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    /// Initialise from XR_KHR_D3D11_enable binding.
    XrResult InitD3D11(const XrGraphicsBindingD3D11KHR* binding);

    /// Initialise from XR_KHR_D3D12_enable binding.
    /// Creates an internal D3D11 device; swapchain textures are shared with
    /// the app's D3D12 device via NT shared handles.
    XrResult InitD3D12(void* d3d12Device, void* d3d12Queue);

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
    GraphicsApi                  m_graphicsApi{GraphicsApi::D3D11};

    // D3D11 device — always valid (provided by app for D3D11, created
    // internally for D3D12 so swapchain SRVs work for our presenter)
    Microsoft::WRL::ComPtr<ID3D11Device>        m_d3d11Dev;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3d11Ctx;

    // D3D12 mode: app's device used to open shared texture handles
    void* m_d3d12Device = nullptr; // ID3D12Device* (raw, app owns lifetime)

    // Stereo presenter — windowed D3D9 + packed stereo surface approach
    NvapiStereoPresenter m_presenter;

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

    // Mouse-look state (accumulated from presenter, applied in LocateViews)
    float m_yaw   = 0.f;  // radians, positive = right
    float m_pitch = 0.f;  // radians, positive = up
    // FOV is driven by the presenter (wheel input + config). Read via GetFov().
    // No separate m_fov needed — presenter is the single source of truth.

    // Eye identification stored during EndFrame
    XrSwapchain  m_leftSwapchain  = XR_NULL_HANDLE;
    XrSwapchain  m_rightSwapchain = XR_NULL_HANDLE;
};

} // namespace xr3dv
