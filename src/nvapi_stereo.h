//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <d3d9.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

// nvapi.h defines StereoHandle, NVDX_ObjectHandle, etc.
// It must be included before any NVAPI type usage.
#include <nvapi.h>

namespace xr3dv {

struct Config;

/// Encapsulates an NVAPI stereo D3D9 presenter.
/// Lifetime: one per XrSession.
class NvapiStereoPresenter {
public:
    NvapiStereoPresenter() = default;
    ~NvapiStereoPresenter();

    NvapiStereoPresenter(const NvapiStereoPresenter&)            = delete;
    NvapiStereoPresenter& operator=(const NvapiStereoPresenter&) = delete;

    /// Create hidden D3D9 window + device and initialise NVAPI stereo.
    /// @param width      Per-eye render width
    /// @param height     Per-eye render height
    /// @param separation Initial separation [0–100]
    /// @param convergence Initial convergence [0–25]
    bool Init(uint32_t width, uint32_t height,
              float separation, float convergence);

    /// Present a stereo frame.
    /// @param leftSRV   D3D11 SRV of the left-eye swapchain image
    /// @param rightSRV  D3D11 SRV of the right-eye swapchain image
    /// @param d3d11Dev  The D3D11 device that owns the SRVs
    bool PresentStereoFrame(
        ID3D11ShaderResourceView* leftSRV,
        ID3D11ShaderResourceView* rightSRV,
        ID3D11Device*             d3d11Dev);

    /// Update separation at runtime (hot-reload).
    void SetSeparation(float pct);

    /// Update convergence at runtime (hot-reload).
    void SetConvergence(float val);

    bool IsInitialised() const { return m_initialised; }

private:
    bool CreateD3D9Device(uint32_t width, uint32_t height);
    bool EnableNvStereo();
    bool CreateStagingResources(uint32_t width, uint32_t height);

    /// Read-back D3D11 texture to CPU then upload to D3D9 surface.
    bool BlitD3D11ToD3D9(
        ID3D11ShaderResourceView* srv,
        ID3D11Device*             d3d11Dev,
        IDirect3DSurface9*        dst);

    // ------ D3D9 objects -------------------------------------------------
    HWND                                     m_hwnd           = nullptr;
    Microsoft::WRL::ComPtr<IDirect3D9Ex>     m_d3d9;
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> m_device;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_leftSurface;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_rightSurface;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_backBuffer;

    // ------ D3D11 staging texture ----------------------------------------
    Microsoft::WRL::ComPtr<ID3D11Texture2D>    m_stagingTex;
    uint32_t m_stagingWidth  = 0;
    uint32_t m_stagingHeight = 0;

    // ------ NVAPI ---------------------------------------------------------
    StereoHandle m_stereoHandle  = nullptr;   ///< NVAPI stereo handle

    // ------ State ---------------------------------------------------------
    bool     m_initialised   = false;
    uint32_t m_width         = 0;
    uint32_t m_height        = 0;
};

/// Query whether the NVAPI stereo path is available on this machine.
bool NvapiIsAvailable();

} // namespace xr3dv
