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

class NvapiStereoPresenter {
public:
    NvapiStereoPresenter() = default;
    ~NvapiStereoPresenter();

    NvapiStereoPresenter(const NvapiStereoPresenter&)            = delete;
    NvapiStereoPresenter& operator=(const NvapiStereoPresenter&) = delete;

    /// Create FSE D3D9 device and initialise NVAPI stereo.
    /// @param fseRate      Full monitor refresh Hz for D3D9 FSE device
    /// @param gameIniPath  Per-game ini for hotkey-save (may be "")
    bool Init(uint32_t width, uint32_t height,
              uint32_t fseRate,
              float separation, float convergence,
              const std::string& gameIniPath = "");

    bool PresentStereoFrame(
        ID3D11ShaderResourceView* leftSRV,
        ID3D11ShaderResourceView* rightSRV,
        ID3D11Device*             d3d11Dev);

    /// Called by PollConfigThread only when INI actually changed.
    void SetSeparation(float pct);
    void SetConvergence(float val);

    float GetSeparation()  const { return m_separation;  }
    float GetConvergence() const { return m_convergence; }
    bool  IsInitialised()  const { return m_initialised; }

private:
    // Stubs — all real work is in MsgThreadProc
    bool CreateD3D9Device(uint32_t, uint32_t, uint32_t);
    bool EnableNvStereo();
    bool CreateStagingResources(uint32_t, uint32_t);

    void MsgThreadProc();

    bool BlitD3D11ToD3D9(
        ID3D11ShaderResourceView*                    srv,
        ID3D11Device*                                d3d11Dev,
        IDirect3DSurface9*                           dst,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>&     stagingTex,
        Microsoft::WRL::ComPtr<IDirect3DSurface9>&   sysMemSurf);

    // ------ D3D9 objects -------------------------------------------------
    HWND                                       m_hwnd        = nullptr;
    HWND                                       m_gameHwnd    = nullptr; // game's main window
    Microsoft::WRL::ComPtr<IDirect3D9Ex>       m_d3d9;
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> m_device;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_leftSurface;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_rightSurface;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_backBuffer; // mono, SetActiveEye redirects it

    // ------ D3D11 staging textures (one per eye, cached) -----------------
    Microsoft::WRL::ComPtr<ID3D11Texture2D>    m_stagingTex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>    m_stagingTexRight;
    uint32_t    m_stagingWidth  = 0;
    uint32_t    m_stagingHeight = 0;
    DXGI_FORMAT m_stagingFormat = DXGI_FORMAT_UNKNOWN;

    // Cached SYSMEM surfaces
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_sysMemLeft;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_sysMemRight;

    // ------ NVAPI ---------------------------------------------------------
    StereoHandle m_stereoHandle = nullptr;

    // ------ Message pump thread -------------------------------------------
    std::thread       m_msgThread;
    std::atomic<bool> m_msgStop{false};
    std::atomic<bool> m_initDone{false};
    std::atomic<bool> m_initOk{false};

    // Timer IDs used in message loop
    static constexpr UINT_PTR TIMER_INPUT_POLL  = 1; // 50 ms — hotkey hold
    static constexpr UINT_PTR TIMER_AUDIO_KEEP  = 2; // 2000 ms — keep game audio alive
    static constexpr UINT_PTR TIMER_STEREO_SYNC = 3; // 500 ms  — sync sep/conv from 3DV OSD

    // Hotkey step sizes
    static constexpr float kSepStep  = 1.0f;
    static constexpr float kConvStep = 0.1f;

    // ------ State ---------------------------------------------------------
    bool        m_initialised = false;
    uint32_t    m_width       = 0;
    uint32_t    m_height      = 0;
    uint32_t    m_fseRate     = 120;
    float       m_separation  = 50.0f;
    float       m_convergence = 5.0f;
    std::string m_gameIniPath;
};

bool NvapiIsAvailable();

} // namespace xr3dv
