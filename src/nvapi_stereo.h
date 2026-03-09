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

// NV Packed-Stereo header — kept for reference / fallback experiments.
// Primary path uses NvAPI_Stereo_SetActiveEye instead.
#define NVSTEREO_IMAGE_SIGNATURE 0x4433445C
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
              bool swapEyes = false,
              const std::string& gameIniPath = "");

    bool PresentStereoFrame(
        ID3D11ShaderResourceView* leftSRV,
        ID3D11ShaderResourceView* rightSRV,
        ID3D11Device*             d3d11Dev);

    void SetSeparation(float pct);
    void SetConvergence(float val);

    float GetSeparation()  const { return m_separation;  }
    float GetConvergence() const { return m_convergence; }
    bool  IsInitialised()  const { return m_initialised; }

private:
    void MsgThreadProc();

    /// Blit one D3D11 SRV → D3D9 DEFAULT surface via CPU staging.
    bool BlitD3D11ToSurface(
        ID3D11ShaderResourceView*              srv,
        ID3D11Device*                          d3d11Dev,
        IDirect3DSurface9*                     dst,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& stagingTex,
        Microsoft::WRL::ComPtr<IDirect3DSurface9>& sysMemSurf);

    // ------ D3D9 objects -------------------------------------------------
    HWND                                       m_hwnd     = nullptr;
    HWND                                       m_gameHwnd = nullptr;
    Microsoft::WRL::ComPtr<IDirect3D9Ex>       m_d3d9;
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> m_device;

    // Per-eye DEFAULT pool surfaces (SetActiveEye routes StretchRect to correct plane)
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_leftSurface;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_rightSurface;
    // Mono backbuffer — SetActiveEye redirects writes to left/right stereo plane
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_backBuffer;

    // ------ D3D11 staging (one per eye, cached) --------------------------
    Microsoft::WRL::ComPtr<ID3D11Texture2D>    m_stagingLeft;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>    m_stagingRight;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_sysMemLeft;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_sysMemRight;
    uint32_t    m_stagingWidth  = 0;
    uint32_t    m_stagingHeight = 0;
    DXGI_FORMAT m_stagingFormat = DXGI_FORMAT_UNKNOWN;

    // ------ NVAPI ---------------------------------------------------------
    StereoHandle m_stereoHandle = nullptr;

    // ------ Message-pump thread -------------------------------------------
    std::thread       m_msgThread;
    std::atomic<bool> m_msgStop{false};
    std::atomic<bool> m_initDone{false};
    std::atomic<bool> m_initOk{false};

    static constexpr UINT_PTR TIMER_INPUT_POLL  = 1; // 50 ms
    static constexpr UINT_PTR TIMER_STEREO_SYNC = 2; // 500 ms

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
