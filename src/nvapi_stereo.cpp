//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  NVAPI stereo bridge: creates a fullscreen-exclusive D3D9Ex device,
//  enables NVAPI stereo mode, and blits D3D11 left/right eye textures
//  into it each frame.  FSE is required for 3D Vision on drivers >= 426.06.

#include "pch.h"
#include "nvapi_stereo.h"
#include "logging.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace xr3dv {

// ---------------------------------------------------------------------------
// Window for FSE D3D9 presentation
// ---------------------------------------------------------------------------
static HWND CreatePresentationWindow() {
    static const wchar_t kClass[] = L"XR3DV_D3D9";

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    // For FSE the window dimensions don't matter; D3D9 takes over the display.
    HWND hw = CreateWindowExW(
        0, kClass, L"XR3DV", WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, wc.hInstance, nullptr);

    ShowWindow(hw, SW_SHOWNOACTIVATE);
    return hw;
}

// ---------------------------------------------------------------------------
// Dedicated Win32 message-pump thread
// ---------------------------------------------------------------------------
void NvapiStereoPresenter::MsgThreadProc() {
    // Messages for m_hwnd must be pumped on the thread that created it.
    while (!m_msgStop.load(std::memory_order_relaxed)) {
        MSG msg;
        if (MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT,
                                         MWMO_INPUTAVAILABLE) != WAIT_TIMEOUT) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { m_msgStop = true; return; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// NvapiStereoPresenter::Init
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::Init(uint32_t width, uint32_t height,
                                 uint32_t frameRate,
                                 float separation, float convergence)
{
    m_width  = width;
    m_height = height;

    // ------ 1. Initialise NVAPI ------------------------------------------
    NvAPI_Status nvs = NvAPI_Initialize();
    if (nvs != NVAPI_OK) {
        NvAPI_ShortString errStr;
        NvAPI_GetErrorMessage(nvs, errStr);
        LOG_ERROR("NvAPI_Initialize failed: %s", errStr);
        return false;
    }

    // ------ 2. Enable stereo globally ------------------------------------
    nvs = NvAPI_Stereo_Enable();
    if (nvs != NVAPI_OK) {
        LOG_ERROR("NvAPI_Stereo_Enable failed (%d). "
                  "Ensure 3D Vision is enabled in NVIDIA Control Panel.", nvs);
        return false;
    }

    // ------ 3. Create window on the message thread -----------------------
    // Win32 requires messages to be pumped on the thread that created the
    // window.  Spawn the thread, let it create the window, then wait.
    m_msgStop = false;
    m_msgThread = std::thread([this, width, height, frameRate]() {
        m_hwnd = CreatePresentationWindow();
        if (m_hwnd) MsgThreadProc();
    });

    for (int i = 0; i < 200 && !m_hwnd; ++i) Sleep(10);
    if (!m_hwnd) {
        LOG_ERROR("Timed out waiting for D3D9 presentation window");
        m_msgStop = true;
        if (m_msgThread.joinable()) m_msgThread.join();
        return false;
    }
    LOG_INFO("D3D9 presentation window HWND=%p", (void*)m_hwnd);

    // ------ 4. Create FSE D3D9Ex device ----------------------------------
    if (!CreateD3D9Device(width, height, frameRate)) return false;

    // ------ 5. Create NVAPI stereo handle --------------------------------
    if (!EnableNvStereo()) return false;

    // ------ 6. Create per-eye offscreen surfaces -------------------------
    if (!CreateStagingResources(width, height)) return false;

    // ------ 7. Apply initial parameters ----------------------------------
    SetSeparation(separation);
    SetConvergence(convergence);

    m_initialised = true;
    LOG_INFO("NvapiStereoPresenter initialised (%ux%u@%uHz FSE, sep=%.1f, conv=%.2f)",
             width, height, frameRate, separation, convergence);
    return true;
}

// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::CreateD3D9Device(uint32_t width, uint32_t height,
                                             uint32_t frameRate) {
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &m_d3d9);
    if (FAILED(hr)) { LOG_ERROR("Direct3DCreate9Ex failed: 0x%08X", hr); return false; }

    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth            = width;
    pp.BackBufferHeight           = height;
    pp.BackBufferFormat           = D3DFMT_A8R8G8B8;
    pp.BackBufferCount            = 2;
    pp.SwapEffect                 = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow              = m_hwnd;
    pp.Windowed                   = FALSE;           // FULLSCREEN EXCLUSIVE
    pp.EnableAutoDepthStencil     = FALSE;
    pp.FullScreen_RefreshRateInHz = frameRate;       // must be 120 for 3D Vision
    // INTERVAL_ONE works correctly in FSE since DWM is bypassed entirely.
    pp.PresentationInterval       = D3DPRESENT_INTERVAL_ONE;

    hr = m_d3d9->CreateDeviceEx(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, m_hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &pp, nullptr, &m_device);

    if (FAILED(hr)) {
        LOG_ERROR("CreateDeviceEx (FSE %ux%u@%uHz) failed: 0x%08X "
                  "-- confirm monitor supports this mode and 3D Vision is on",
                  width, height, frameRate, hr);
        return false;
    }
    LOG_INFO("D3D9 FSE device: %ux%u@%uHz", width, height, frameRate);
    return true;
}

// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::EnableNvStereo() {
    NvAPI_Status nvs = NvAPI_Stereo_CreateHandleFromIUnknown(m_device.Get(), &m_stereoHandle);
    if (nvs != NVAPI_OK) {
        LOG_ERROR("NvAPI_Stereo_CreateHandleFromIUnknown failed (%d)", nvs);
        return false;
    }
    // Activate stereo
    nvs = NvAPI_Stereo_Activate(m_stereoHandle);
    if (nvs != NVAPI_OK) {
        LOG_ERROR("NvAPI_Stereo_Activate failed (%d)", nvs);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::CreateStagingResources(uint32_t width, uint32_t height) {
    HRESULT hr;

    // Offscreen plain surfaces for each eye (D3DPOOL_DEFAULT)
    hr = m_device->CreateOffscreenPlainSurface(
        width, height, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        &m_leftSurface, nullptr);
    if (FAILED(hr)) { LOG_ERROR("CreateOffscreenPlainSurface (left) failed: 0x%08X", hr); return false; }

    hr = m_device->CreateOffscreenPlainSurface(
        width, height, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        &m_rightSurface, nullptr);
    if (FAILED(hr)) { LOG_ERROR("CreateOffscreenPlainSurface (right) failed: 0x%08X", hr); return false; }

    // Back buffer reference for StretchRect
    hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_backBuffer);
    if (FAILED(hr)) { LOG_ERROR("GetBackBuffer failed: 0x%08X", hr); return false; }

    return true;
}

// ---------------------------------------------------------------------------
// PresentStereoFrame
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::PresentStereoFrame(
    ID3D11ShaderResourceView* leftSRV,
    ID3D11ShaderResourceView* rightSRV,
    ID3D11Device*             d3d11Dev)
{
    if (!m_initialised) return false;

    LOG_TRACE("PresentStereoFrame: enter");

    // Blit left eye --------------------------------------------------------
    LOG_TRACE("PresentStereoFrame: blitting left eye...");
    if (!BlitD3D11ToD3D9(leftSRV, d3d11Dev, m_leftSurface.Get(),
                          m_stagingTex, m_sysMemLeft)) return false;
    LOG_TRACE("PresentStereoFrame: left blit done, calling StretchRect...");
    NvAPI_Stereo_SetActiveEye(m_stereoHandle, NVAPI_STEREO_EYE_LEFT);
    m_device->StretchRect(m_leftSurface.Get(), nullptr, m_backBuffer.Get(), nullptr, D3DTEXF_LINEAR);
    LOG_TRACE("PresentStereoFrame: left StretchRect done");

    // Blit right eye -------------------------------------------------------
    LOG_TRACE("PresentStereoFrame: blitting right eye...");
    if (!BlitD3D11ToD3D9(rightSRV, d3d11Dev, m_rightSurface.Get(),
                          m_stagingTexRight, m_sysMemRight)) return false;
    LOG_TRACE("PresentStereoFrame: right blit done, calling StretchRect...");
    NvAPI_Stereo_SetActiveEye(m_stereoHandle, NVAPI_STEREO_EYE_RIGHT);
    m_device->StretchRect(m_rightSurface.Get(), nullptr, m_backBuffer.Get(), nullptr, D3DTEXF_LINEAR);
    LOG_TRACE("PresentStereoFrame: right StretchRect done");

    // Present the stereo frame ---------------------------------------------
    LOG_TRACE("PresentStereoFrame: calling PresentEx...");
    HRESULT hr = m_device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    if (FAILED(hr)) {
        LOG_ERROR("D3D9 PresentEx failed: 0x%08X", hr);
        return false;
    }
    LOG_TRACE("PresentStereoFrame: PresentEx done hr=0x%08X", (unsigned)hr);
    return true;
}

// ---------------------------------------------------------------------------
// BlitD3D11ToD3D9 — CPU readback path
//
// GPU-to-GPU copy between D3D11 and D3D9 is not directly possible without
// a shared resource handle.  We use a D3D11 STAGING texture for CPU read-
// back, then lock a D3D9 SYSMEM surface and memcpy.
//
// For production use, replace with DXGI interop (IDXGIKeyedMutex / shared
// handles) to avoid the CPU copy and its latency.
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::BlitD3D11ToD3D9(
    ID3D11ShaderResourceView*                  srv,
    ID3D11Device*                              d3d11Dev,
    IDirect3DSurface9*                         dst,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>&   stagingTex,
    Microsoft::WRL::ComPtr<IDirect3DSurface9>& sysMemSurf)
{
    // ---- Resolve the SRV to a texture ------------------------------------
    ComPtr<ID3D11Resource> res;
    srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> srcTex;
    if (FAILED(res.As(&srcTex))) {
        LOG_ERROR("BlitD3D11ToD3D9: SRV resource is not a Texture2D");
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    srcTex->GetDesc(&srcDesc);

    // ---- Ensure staging texture exists and matches dims AND format -------
    if (!stagingTex ||
        m_stagingWidth  != srcDesc.Width ||
        m_stagingHeight != srcDesc.Height ||
        m_stagingFormat != srcDesc.Format)
    {
        stagingTex.Reset();
        sysMemSurf.Reset();  // invalidate paired SYSMEM surface
        m_stagingWidth  = srcDesc.Width;
        m_stagingHeight = srcDesc.Height;
        m_stagingFormat = srcDesc.Format;

        D3D11_TEXTURE2D_DESC stDesc{};
        stDesc.Width          = srcDesc.Width;
        stDesc.Height         = srcDesc.Height;
        stDesc.MipLevels      = 1;
        stDesc.ArraySize      = 1;
        stDesc.Format         = srcDesc.Format; // must match exactly for CopyResource
        stDesc.SampleDesc     = {1, 0};
        stDesc.Usage          = D3D11_USAGE_STAGING;
        stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        if (FAILED(d3d11Dev->CreateTexture2D(&stDesc, nullptr, &stagingTex))) {
            LOG_ERROR("Failed to create D3D11 staging texture");
            return false;
        }
        LOG_VERBOSE("Staging texture (re)created: %ux%u fmt=%u",
                    m_stagingWidth, m_stagingHeight, (unsigned)m_stagingFormat);
    }

    // ---- Ensure SYSMEM surface is allocated (cached, not per-frame) ------
    if (!sysMemSurf) {
        HRESULT hrd = m_device->CreateOffscreenPlainSurface(
            m_stagingWidth, m_stagingHeight,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
            &sysMemSurf, nullptr);
        if (FAILED(hrd)) {
            LOG_ERROR("CreateOffscreenPlainSurface (sysmem) failed: 0x%08X", hrd);
            return false;
        }
    }

    // ---- Copy D3D11 tex -> staging and block until GPU is done -----------
    ComPtr<ID3D11DeviceContext> ctx;
    d3d11Dev->GetImmediateContext(&ctx);
    LOG_TRACE("Blit: CopyResource fmt=%u %ux%u", (unsigned)m_stagingFormat, m_stagingWidth, m_stagingHeight);
    ctx->CopyResource(stagingTex.Get(), srcTex.Get());

    // Blocking Map: stalls CPU until CopyResource completes (~0.5–2 ms for
    // a 1080p texture on a modern GPU).  DO_NOT_WAIT was wrong here because
    // the staging texture has no fence — the driver must guarantee completion
    // before Map returns, but only with MapFlags=0.
    LOG_TRACE("Blit: Map (blocking)...");
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        LOG_ERROR("D3D11 Map staging failed: 0x%08X", hr);
        return false;
    }
    LOG_TRACE("Blit: Map done rowPitch=%u", mapped.RowPitch);

    // ---- Copy row-by-row into cached SYSMEM surface ----------------------
    D3DLOCKED_RECT lr{};
    HRESULT hrd = sysMemSurf->LockRect(&lr, nullptr, D3DLOCK_DISCARD);
    if (FAILED(hrd)) {
        ctx->Unmap(stagingTex.Get(), 0);
        LOG_ERROR("LockRect failed: 0x%08X", hrd);
        return false;
    }

    const uint8_t* src  = reinterpret_cast<const uint8_t*>(mapped.pData);
    uint8_t*       dstP = reinterpret_cast<uint8_t*>(lr.pBits);
    size_t rowBytes = static_cast<size_t>(m_stagingWidth) * 4;

    for (uint32_t row = 0; row < m_stagingHeight; ++row) {
        memcpy(dstP + row * lr.Pitch,
               src  + row * mapped.RowPitch,
               rowBytes);
    }

    sysMemSurf->UnlockRect();
    ctx->Unmap(stagingTex.Get(), 0);

    // ---- UpdateSurface: SYSMEM -> DEFAULT pool ---------------------------
    hrd = m_device->UpdateSurface(sysMemSurf.Get(), nullptr, dst, nullptr);
    if (FAILED(hrd)) {
        LOG_ERROR("UpdateSurface failed: 0x%08X", hrd);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Hot-reload helpers
// ---------------------------------------------------------------------------
void NvapiStereoPresenter::SetSeparation(float pct) {
    if (!m_stereoHandle) return;
    NvAPI_Stereo_SetSeparation(m_stereoHandle, pct);
    LOG_VERBOSE("Stereo separation set to %.1f%%", pct);
}

void NvapiStereoPresenter::SetConvergence(float val) {
    if (!m_stereoHandle) return;
    NvAPI_Stereo_SetConvergence(m_stereoHandle, val);
    LOG_VERBOSE("Stereo convergence set to %.2f", val);
}

// ---------------------------------------------------------------------------
NvapiStereoPresenter::~NvapiStereoPresenter() {
    m_backBuffer.Reset();
    m_leftSurface.Reset();
    m_rightSurface.Reset();
    m_stagingTex.Reset();
    m_stagingTexRight.Reset();
    m_sysMemLeft.Reset();
    m_sysMemRight.Reset();

    if (m_stereoHandle) {
        NvAPI_Stereo_DestroyHandle(m_stereoHandle);
        m_stereoHandle = nullptr;
    }

    m_device.Reset();
    m_d3d9.Reset();

    // Stop the message pump thread before destroying the window.
    m_msgStop = true;
    if (m_hwnd) PostMessageW(m_hwnd, WM_QUIT, 0, 0);
    if (m_msgThread.joinable()) m_msgThread.join();

    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    NvAPI_Unload();
}

// ---------------------------------------------------------------------------
bool NvapiIsAvailable() {
    NvAPI_Status s = NvAPI_Initialize();
    if (s != NVAPI_OK) return false;
    NvU8 isStereoEnabled = 0;
    NvAPI_Stereo_IsEnabled(&isStereoEnabled);
    NvAPI_Unload();
    return isStereoEnabled != 0;
}

} // namespace xr3dv
