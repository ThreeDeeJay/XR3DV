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

// ---------------------------------------------------------------------------
// NV Packed-Stereo surface header (last row of a W x (H*2+1) surface).
// The NVIDIA display driver DDI hook intercepts StretchRect from a surface
// containing this signature and routes it to the left/right stereo planes.
// Works in windowed mode; NvAPI_Stereo_Activate is NOT required.
// ---------------------------------------------------------------------------
#define NVSTEREO_IMAGE_SIGNATURE 0x4433445C
struct NvStereoImageHeader {
    DWORD signature;  // NVSTEREO_IMAGE_SIGNATURE
    DWORD width;      // width of ONE eye in pixels
    DWORD height;     // height of ONE eye in pixels
    DWORD bpp;        // bits per pixel (32)
    DWORD flags;      // 0 = normal, 1 = swap eyes
};

class NvapiStereoPresenter {
public:
    NvapiStereoPresenter() = default;
    ~NvapiStereoPresenter();

    NvapiStereoPresenter(const NvapiStereoPresenter&)            = delete;
    NvapiStereoPresenter& operator=(const NvapiStereoPresenter&) = delete;

    /// Initialise D3D9 windowed device and NVAPI stereo.
    /// @param fseRate  Monitor Hz (used only for log/info; device is windowed)
    bool Init(uint32_t width, uint32_t height,
              uint32_t fseRate,
              float separation, float convergence,
              bool swapEyes = false,
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
    void MsgThreadProc();

    /// Blit one D3D11 eye texture into the packed SYSMEM surface at yOffset.
    /// stagingTex is created/reused per-eye; both are class members.
    bool BlitD3D11ToPacked(
        ID3D11ShaderResourceView*              srv,
        ID3D11Device*                          d3d11Dev,
        uint32_t                               yOffset,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& stagingTex);

    // ------ D3D9 objects -------------------------------------------------
    HWND                                       m_hwnd   = nullptr;
    HWND                                       m_gameHwnd = nullptr;
    Microsoft::WRL::ComPtr<IDirect3D9Ex>       m_d3d9;
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> m_device;

    // Packed stereo composite surface: W x (H*2+1)
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_packedSysMem;   // SYSMEM — CPU writable
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_packedDefault;  // DEFAULT — GPU StretchRect src

    // Normal W x H mono backbuffer (StretchRect destination)
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_backBuffer;

    // ------ D3D11 staging textures (one per eye, cached) -----------------
    Microsoft::WRL::ComPtr<ID3D11Texture2D>    m_stagingLeft;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>    m_stagingRight;
    uint32_t    m_stagingWidth  = 0;
    uint32_t    m_stagingHeight = 0;
    DXGI_FORMAT m_stagingFormat = DXGI_FORMAT_UNKNOWN;

    // ------ NVAPI (for SetSeparation / SetConvergence / stereo sync) ------
    StereoHandle m_stereoHandle = nullptr;

    // ------ Message-pump thread -------------------------------------------
    std::thread       m_msgThread;
    std::atomic<bool> m_msgStop{false};
    std::atomic<bool> m_initDone{false};
    std::atomic<bool> m_initOk{false};

    // Timer IDs
    static constexpr UINT_PTR TIMER_INPUT_POLL  = 1; // 50 ms — hotkey hold detection
    static constexpr UINT_PTR TIMER_STEREO_SYNC = 2; // 500 ms — sync sep/conv from 3DV OSD

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
    bool        m_swapEyes    = false;
    std::string m_gameIniPath;
};

bool NvapiIsAvailable();

} // namespace xr3dv
