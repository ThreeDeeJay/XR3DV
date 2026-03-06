#pragma once

#include <d3d9.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <thread>
#include <atomic>

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

    /// Create FSE D3D9 device and initialise NVAPI stereo.
    /// @param width       Per-eye render width
    /// @param height      Per-eye render height
    /// @param frameRate   Display refresh rate (Hz) — must be 120 for 3D Vision
    /// @param separation  Initial separation [0–100]
    /// @param convergence Initial convergence [0–25]
    bool Init(uint32_t width, uint32_t height, uint32_t frameRate,
              float separation, float convergence);

    /// Present a stereo frame.
    bool PresentStereoFrame(
        ID3D11ShaderResourceView* leftSRV,
        ID3D11ShaderResourceView* rightSRV,
        ID3D11Device*             d3d11Dev);

    void SetSeparation(float pct);
    void SetConvergence(float val);
    bool IsInitialised() const { return m_initialised; }

private:
    bool CreateD3D9Device(uint32_t width, uint32_t height, uint32_t frameRate);
    bool EnableNvStereo();
    bool CreateStagingResources(uint32_t width, uint32_t height);

    /// Dedicated Win32 message-pump thread: keeps the FSE window alive.
    void MsgThreadProc();

    bool BlitD3D11ToD3D9(
        ID3D11ShaderResourceView*                    srv,
        ID3D11Device*                                d3d11Dev,
        IDirect3DSurface9*                           dst,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>&     stagingTex,
        Microsoft::WRL::ComPtr<IDirect3DSurface9>&   sysMemSurf);

    // ------ D3D9 objects -------------------------------------------------
    HWND                                       m_hwnd           = nullptr;
    Microsoft::WRL::ComPtr<IDirect3D9Ex>       m_d3d9;
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> m_device;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_leftSurface;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_rightSurface;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_backBuffer;

    // ------ D3D11 staging textures (one per eye, cached) -----------------
    Microsoft::WRL::ComPtr<ID3D11Texture2D>    m_stagingTex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>    m_stagingTexRight;
    uint32_t    m_stagingWidth  = 0;
    uint32_t    m_stagingHeight = 0;
    DXGI_FORMAT m_stagingFormat = DXGI_FORMAT_UNKNOWN;

    // Cached SYSMEM surfaces for CPU→D3D9 upload
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_sysMemLeft;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  m_sysMemRight;

    // ------ NVAPI ---------------------------------------------------------
    StereoHandle m_stereoHandle = nullptr;

    // ------ Message pump thread -------------------------------------------
    std::thread       m_msgThread;
    std::atomic<bool> m_msgStop{false};

    // ------ State ---------------------------------------------------------
    bool     m_initialised = false;
    uint32_t m_width       = 0;
    uint32_t m_height      = 0;
};

bool NvapiIsAvailable();

} // namespace xr3dv
