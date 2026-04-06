//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <d3d9.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <thread>
#include <atomic>

#include <nvapi.h>

namespace xr3dv {

#define NVSTEREO_IMAGE_SIGNATURE 0x4433564e
struct NvStereoImageHeader {
    DWORD signature;
    DWORD width;
    DWORD height;
    DWORD bpp;
    DWORD flags;   // 0 = normal, 1 = swap eyes
};

class NvapiStereoPresenter {
public:
    NvapiStereoPresenter() = default;
    ~NvapiStereoPresenter();

    NvapiStereoPresenter(const NvapiStereoPresenter&)            = delete;
    NvapiStereoPresenter& operator=(const NvapiStereoPresenter&) = delete;

    bool Init(uint32_t width, uint32_t height,
              uint32_t fseRate,
              float separation, float convergence,
              float fov = 45.0f,
              bool swapEyes = false,
              bool forcePopup = false,
              const std::string& gameIniPath = "");

    bool PresentStereoFrame(
        ID3D11ShaderResourceView* leftSRV,
        ID3D11ShaderResourceView* rightSRV,
        ID3D11Device*             d3d11Dev);

    void SetSeparation(float pct);
    void SetConvergence(float val);
    void SetFov(float deg) { m_fov = std::max(10.0f, std::min(deg, 89.0f)); }

    float GetSeparation()  const { return m_separation;  }
    float GetConvergence() const { return m_convergence; }
    float GetFov()         const { return m_fov; }
    bool  IsInitialised()  const { return m_initialised; }

    // Mouse-look: consumed each frame by Session::LocateViews.
    void ConsumeDelta(int32_t& dx, int32_t& dy, bool& recenter) {
        dx       = m_mouseDeltaX.exchange(0, std::memory_order_relaxed);
        dy       = m_mouseDeltaY.exchange(0, std::memory_order_relaxed);
        recenter = m_recenterRequested.exchange(false, std::memory_order_relaxed);
    }

    // FOV wheel: consumed each frame by Session::LocateViews.
    // Returns accumulated wheel notches (positive = scroll up = increase FOV).
    int32_t ConsumeFovDelta() {
        return m_fovWheelDelta.exchange(0, std::memory_order_relaxed);
    }

    // Written by PopupWndProc (free function) via WM_INPUT — must be public.
    std::atomic<int32_t> m_mouseDeltaX{0};
    std::atomic<int32_t> m_mouseDeltaY{0};
    std::atomic<bool>    m_recenterRequested{false};
    std::atomic<int32_t> m_fovWheelDelta{0}; // wheel notches (+120 per detent up)

private:
    void MsgThreadProc();

    bool BlitD3D11ToSurface(
        ID3D11ShaderResourceView*               srv,
        ID3D11Device*                           d3d11Dev,
        IDirect3DSurface9*                      dstDefault,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& stagingTex,
        Microsoft::WRL::ComPtr<IDirect3DSurface9>& sysMemSurf);

    bool BlitD3D11ToPacked(
        ID3D11ShaderResourceView*               srv,
        ID3D11Device*                           d3d11Dev,
        uint32_t                                yOffset,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& stagingTex);

    // ------ D3D9 objects -------------------------------------------------
    HWND                                       m_hwnd      = nullptr; // device window (always popup)
    HWND                                       m_ownedHwnd = nullptr; // same — we always own it
    HWND                                       m_gameHwnd  = nullptr; // game's main window
    Microsoft::WRL::ComPtr<IDirect3D9Ex>       m_d3d9;
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> m_device;

    // PATH A: per-eye surfaces for SetActiveEye
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_leftSurface;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_rightSurface;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_sysMemLeft;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_sysMemRight;

    // PATH B: packed W×(2H+1) fallback
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_packedSysMem;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_packedDefault;

    // Mono backbuffer
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_backBuffer;

    // ------ D3D11 staging ------------------------------------------------
    Microsoft::WRL::ComPtr<ID3D11Texture2D>    m_stagingLeft;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>    m_stagingRight;
    uint32_t    m_stagingWidth  = 0;
    uint32_t    m_stagingHeight = 0;
    DXGI_FORMAT m_stagingFormat = DXGI_FORMAT_UNKNOWN;

    // ------ NVAPI ---------------------------------------------------------
    StereoHandle       m_stereoHandle    = nullptr;
    std::atomic<bool>  m_stereoActivated{false};

    // ------ Mouse-look centre (written by msg thread only) ---------------
    int32_t m_centerX = 0;
    int32_t m_centerY = 0;

    // ------ Message-pump thread -------------------------------------------
    std::thread       m_msgThread;
    std::atomic<bool> m_msgStop{false};
    std::atomic<bool> m_initDone{false};
    std::atomic<bool> m_initOk{false};
    DWORD             m_msgThreadId = 0;

    static constexpr UINT_PTR TIMER_INPUT_POLL  = 1;
    static constexpr UINT_PTR TIMER_STEREO_SYNC = 2;

    static constexpr float kSepStep  = 1.0f;
    static constexpr float kConvStep = 0.1f;

    // ------ State ---------------------------------------------------------
    bool        m_initialised = false;
    uint32_t    m_width       = 0;
    uint32_t    m_height      = 0;
    uint32_t    m_fseRate     = 120;
    float       m_separation  = 50.0f;
    float       m_convergence = 5.0f;
    float       m_fov         = 45.0f;
    bool        m_swapEyes    = false;
    bool        m_forcePopup  = false;
    std::string m_gameIniPath;
};

bool NvapiIsAvailable();

} // namespace xr3dv
