//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  NVAPI stereo bridge: creates a fullscreen-exclusive D3D9Ex device,
//  enables NVAPI stereo mode, and blits D3D11 left/right eye textures.
//  FSE is required for 3D Vision on drivers >= 426.06.

#include "pch.h"
#include "nvapi_stereo.h"
#include "config.h"
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

    HWND hw = CreateWindowExW(
        0, kClass, L"XR3DV", WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, wc.hInstance, nullptr);
    // Caller is responsible for showing/activating
    return hw;
}

// ---------------------------------------------------------------------------
// Message + D3D9 thread — ALL D3D9 FSE work happens here
// ---------------------------------------------------------------------------
void NvapiStereoPresenter::MsgThreadProc() {
    // --- Save the current foreground window so we can restore focus later --
    m_savedFgWnd = GetForegroundWindow();

    // --- Create and ACTIVATE the window ---
    m_hwnd = CreatePresentationWindow();
    if (!m_hwnd) {
        LOG_ERROR("CreatePresentationWindow failed (GLE=%u)", GetLastError());
        m_initOk.store(false); m_initDone.store(true); return;
    }
    ShowWindow(m_hwnd, SW_SHOW);
    BringWindowToTop(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);

    // Pump messages briefly to let the window reach foreground
    MSG tmp;
    for (int i = 0; i < 5; ++i) {
        Sleep(20);
        while (PeekMessageW(&tmp, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&tmp); DispatchMessageW(&tmp);
        }
    }
    LOG_INFO("D3D9 window HWND=%p foreground=%s active=%s",
             (void*)m_hwnd,
             GetForegroundWindow() == m_hwnd ? "YES" : "NO",
             GetActiveWindow()     == m_hwnd ? "YES" : "NO");

    // --- Create D3D9Ex ---
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &m_d3d9);
    if (FAILED(hr)) {
        LOG_ERROR("Direct3DCreate9Ex failed: 0x%08X", hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }

    // --- Enumerate adapter modes (log + verify target mode exists) ---
    {
        UINT cnt = m_d3d9->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
        LOG_INFO("Adapter modes (X8R8G8B8): %u", cnt);
        bool found = false;
        for (UINT i = 0; i < cnt; ++i) {
            D3DDISPLAYMODE m{};
            m_d3d9->EnumAdapterModes(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8, i, &m);
            bool match = (m.Width == m_width && m.Height == m_height
                          && m.RefreshRate == m_fseRate);
            LOG_VERBOSE("  [%u] %ux%u@%uHz%s", i, m.Width, m.Height, m.RefreshRate,
                        match ? " <-- TARGET" : "");
            if (match) found = true;
        }
        if (!found)
            LOG_ERROR("No adapter mode matching %ux%u@%uHz -- check MonitorRate in xr3dv.ini",
                      m_width, m_height, m_fseRate);
    }

    // --- Create FSE device ---
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth            = m_width;
    pp.BackBufferHeight           = m_height;
    pp.BackBufferFormat           = D3DFMT_X8R8G8B8;
    pp.BackBufferCount            = 2;
    pp.SwapEffect                 = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow              = m_hwnd;
    pp.Windowed                   = FALSE;
    pp.EnableAutoDepthStencil     = FALSE;
    pp.FullScreen_RefreshRateInHz = m_fseRate;
    pp.PresentationInterval       = D3DPRESENT_INTERVAL_ONE;

    D3DDISPLAYMODEEX fsMode{};
    fsMode.Size             = sizeof(D3DDISPLAYMODEEX);
    fsMode.Width            = m_width;
    fsMode.Height           = m_height;
    fsMode.RefreshRate      = m_fseRate;
    fsMode.Format           = D3DFMT_X8R8G8B8;
    fsMode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;

    hr = m_d3d9->CreateDeviceEx(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, m_hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED
        | D3DCREATE_FPU_PRESERVE,
        &pp, &fsMode, &m_device);

    if (FAILED(hr)) {
        LOG_ERROR("CreateDeviceEx (FSE %ux%u@%uHz) failed: 0x%08X",
                  m_width, m_height, m_fseRate, hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }
    LOG_INFO("D3D9 FSE device: %ux%u@%uHz", m_width, m_height, m_fseRate);

    // --- NVAPI stereo handle + activate ---
    NvAPI_Status nvs = NvAPI_Stereo_CreateHandleFromIUnknown(
        m_device.Get(), &m_stereoHandle);
    if (nvs != NVAPI_OK) {
        LOG_ERROR("NvAPI_Stereo_CreateHandleFromIUnknown failed (%d)", nvs);
        m_initOk.store(false); m_initDone.store(true); return;
    }
    nvs = NvAPI_Stereo_Activate(m_stereoHandle);
    if (nvs != NVAPI_OK) {
        LOG_ERROR("NvAPI_Stereo_Activate failed (%d)", nvs);
        m_initOk.store(false); m_initDone.store(true); return;
    }
    {
        NvU8 activated = 0;
        NvAPI_Stereo_IsActivated(m_stereoHandle, &activated);
        LOG_INFO("NVAPI stereo activated: %s", activated ? "YES" : "NO (check Control Panel)");
    }

    // --- Force-stereo surface creation mode so offscreen surfaces get
    //     proper left/right planes recognised by the driver --------------------
    NvAPI_Stereo_SetSurfaceCreationMode(
        m_stereoHandle, NVAPI_STEREO_SURFACECREATEMODE_FORCESTEREO);

    // --- Per-eye offscreen surfaces (DEFAULT pool) ---
    hr = m_device->CreateOffscreenPlainSurface(
        m_width, m_height, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT,
        &m_leftSurface, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("CreateOffscreenPlainSurface (left) failed: 0x%08X", hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }
    hr = m_device->CreateOffscreenPlainSurface(
        m_width, m_height, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT,
        &m_rightSurface, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("CreateOffscreenPlainSurface (right) failed: 0x%08X", hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }

    // Reset surface creation mode to normal for everything else
    NvAPI_Stereo_SetSurfaceCreationMode(
        m_stereoHandle, NVAPI_STEREO_SURFACECREATEMODE_AUTO);

    // --- Get explicit left/right back buffers ---
    // When NVAPI stereo is active the device has separate left/right back
    // buffer planes.  We StretchRect directly to each rather than relying
    // on SetActiveEye to redirect writes to a mono back buffer reference.
    hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_LEFT,  &m_backBufferLeft);
    if (FAILED(hr)) {
        // Fallback: stereo not truly active, use mono back buffer for both
        LOG_ERROR("GetBackBuffer(LEFT) failed 0x%08X -- stereo may not be active", hr);
        m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_backBufferLeft);
        m_backBufferRight = m_backBufferLeft;
    } else {
        hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_RIGHT, &m_backBufferRight);
        if (FAILED(hr)) {
            LOG_ERROR("GetBackBuffer(RIGHT) failed 0x%08X", hr);
            m_backBufferRight = m_backBufferLeft;
        } else {
            LOG_INFO("Stereo back buffers acquired (left=%p right=%p)",
                     (void*)m_backBufferLeft.Get(), (void*)m_backBufferRight.Get());
        }
    }

    // --- Restore focus to the game window ---
    // Taking FSE focus sends WM_ACTIVATE(INACTIVE) to the game, which can
    // pause audio.  Post fake focus-restore messages back to it.
    if (m_savedFgWnd && m_savedFgWnd != m_hwnd) {
        PostMessageW(m_savedFgWnd, WM_ACTIVATE,
                     MAKEWPARAM(WA_ACTIVE, 0), (LPARAM)m_hwnd);
        PostMessageW(m_savedFgWnd, WM_SETFOCUS, (WPARAM)m_hwnd, 0);
        LOG_INFO("Focus restore messages sent to game window %p", (void*)m_savedFgWnd);
    }

    // --- Register hotkeys ---
    RegisterHotKey(nullptr, HK_SEP_DEC,  MOD_CONTROL | MOD_NOREPEAT, VK_F3);
    RegisterHotKey(nullptr, HK_SEP_INC,  MOD_CONTROL | MOD_NOREPEAT, VK_F4);
    RegisterHotKey(nullptr, HK_CONV_DEC, MOD_CONTROL | MOD_NOREPEAT, VK_F5);
    RegisterHotKey(nullptr, HK_CONV_INC, MOD_CONTROL | MOD_NOREPEAT, VK_F6);
    RegisterHotKey(nullptr, HK_SAVE,     MOD_CONTROL | MOD_NOREPEAT, VK_F7);
    LOG_INFO("Hotkeys registered: Ctrl+F3/F4=sep Ctrl+F5/F6=conv Ctrl+F7=save");

    // Signal Init() that setup is complete
    m_initOk.store(true);
    m_initDone.store(true);

    // --- Message pump ---
    while (!m_msgStop.load(std::memory_order_relaxed)) {
        MSG msg;
        if (MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT,
                                         MWMO_INPUTAVAILABLE) != WAIT_TIMEOUT) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { m_msgStop = true; goto done; }

                if (msg.message == WM_HOTKEY) {
                    float sep  = m_separation;
                    float conv = m_convergence;
                    switch (msg.wParam) {
                    case HK_SEP_DEC:
                        sep = std::max(0.0f,   sep  - kSepStep);
                        SetSeparation(sep);
                        break;
                    case HK_SEP_INC:
                        sep = std::min(100.0f, sep  + kSepStep);
                        SetSeparation(sep);
                        break;
                    case HK_CONV_DEC:
                        conv = std::max(0.0f,  conv - kConvStep);
                        SetConvergence(conv);
                        break;
                    case HK_CONV_INC:
                        conv = std::min(25.0f, conv + kConvStep);
                        SetConvergence(conv);
                        break;
                    case HK_SAVE:
                        SaveGameStereoSettings(m_gameIniPath, m_separation, m_convergence);
                        break;
                    }
                }

                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
done:
    UnregisterHotKey(nullptr, HK_SEP_DEC);
    UnregisterHotKey(nullptr, HK_SEP_INC);
    UnregisterHotKey(nullptr, HK_CONV_DEC);
    UnregisterHotKey(nullptr, HK_CONV_INC);
    UnregisterHotKey(nullptr, HK_SAVE);
}

// ---------------------------------------------------------------------------
// NvapiStereoPresenter::Init
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::Init(uint32_t width, uint32_t height,
                                 uint32_t fseRate,
                                 float separation, float convergence,
                                 const std::string& gameIniPath)
{
    m_width        = width;
    m_height       = height;
    m_fseRate      = fseRate;
    m_separation   = separation;
    m_convergence  = convergence;
    m_gameIniPath  = gameIniPath;

    // ------ 1. Initialise NVAPI on calling thread -------------------------
    NvAPI_Status nvs = NvAPI_Initialize();
    if (nvs != NVAPI_OK) {
        NvAPI_ShortString errStr;
        NvAPI_GetErrorMessage(nvs, errStr);
        LOG_ERROR("NvAPI_Initialize failed: %s", errStr);
        return false;
    }
    nvs = NvAPI_Stereo_Enable();
    if (nvs != NVAPI_OK) {
        LOG_ERROR("NvAPI_Stereo_Enable failed (%d). "
                  "Ensure 3D Vision is enabled in NVIDIA Control Panel.", nvs);
        return false;
    }

    // ------ 2. Spin up message thread (does ALL D3D9 init inside) --------
    m_msgStop  = false;
    m_initDone = false;
    m_initOk   = false;
    m_msgThread = std::thread([this]() { MsgThreadProc(); });

    for (int i = 0; i < 500 && !m_initDone.load(); ++i) Sleep(10);
    if (!m_initDone.load()) {
        LOG_ERROR("Timed out waiting for D3D9 init");
        m_msgStop = true;
        if (m_msgThread.joinable()) m_msgThread.join();
        return false;
    }
    if (!m_initOk.load()) {
        LOG_ERROR("D3D9 init failed (see above)");
        m_msgStop = true;
        if (m_msgThread.joinable()) m_msgThread.join();
        return false;
    }

    // ------ 3. Apply initial stereo parameters ---------------------------
    SetSeparation(separation);
    SetConvergence(convergence);

    m_initialised = true;
    LOG_INFO("NvapiStereoPresenter initialised (%ux%u@%uHz FSE, sep=%.1f, conv=%.2f)",
             width, height, fseRate, separation, convergence);
    return true;
}

// ---------------------------------------------------------------------------
// Stubs
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::CreateD3D9Device(uint32_t, uint32_t, uint32_t) { return true; }
bool NvapiStereoPresenter::EnableNvStereo()                                { return true; }
bool NvapiStereoPresenter::CreateStagingResources(uint32_t, uint32_t)     { return true; }

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

    // Blit left eye D3D11 → D3D9 offscreen surface
    LOG_TRACE("PresentStereoFrame: blitting left eye...");
    if (!BlitD3D11ToD3D9(leftSRV, d3d11Dev, m_leftSurface.Get(),
                          m_stagingTex, m_sysMemLeft)) return false;

    // StretchRect directly to the LEFT back buffer plane
    LOG_TRACE("PresentStereoFrame: StretchRect left -> backBufferLeft...");
    m_device->StretchRect(m_leftSurface.Get(), nullptr,
                          m_backBufferLeft.Get(), nullptr, D3DTEXF_LINEAR);

    // Blit right eye
    LOG_TRACE("PresentStereoFrame: blitting right eye...");
    if (!BlitD3D11ToD3D9(rightSRV, d3d11Dev, m_rightSurface.Get(),
                          m_stagingTexRight, m_sysMemRight)) return false;

    // StretchRect directly to the RIGHT back buffer plane
    LOG_TRACE("PresentStereoFrame: StretchRect right -> backBufferRight...");
    m_device->StretchRect(m_rightSurface.Get(), nullptr,
                          m_backBufferRight.Get(), nullptr, D3DTEXF_LINEAR);

    // Present the stereo pair
    LOG_TRACE("PresentStereoFrame: PresentEx...");
    HRESULT hr = m_device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    if (FAILED(hr)) {
        LOG_ERROR("D3D9 PresentEx failed: 0x%08X", hr);
        return false;
    }
    LOG_TRACE("PresentStereoFrame: done hr=0x%08X", (unsigned)hr);
    return true;
}

// ---------------------------------------------------------------------------
// BlitD3D11ToD3D9 — CPU readback path
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::BlitD3D11ToD3D9(
    ID3D11ShaderResourceView*                  srv,
    ID3D11Device*                              d3d11Dev,
    IDirect3DSurface9*                         dst,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>&   stagingTex,
    Microsoft::WRL::ComPtr<IDirect3DSurface9>& sysMemSurf)
{
    ComPtr<ID3D11Resource> res;
    srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> srcTex;
    if (FAILED(res.As(&srcTex))) {
        LOG_ERROR("BlitD3D11ToD3D9: SRV resource is not Texture2D");
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    srcTex->GetDesc(&srcDesc);

    // Invalidate cached staging tex if dims/format changed
    if (!stagingTex ||
        m_stagingWidth  != srcDesc.Width ||
        m_stagingHeight != srcDesc.Height ||
        m_stagingFormat != srcDesc.Format)
    {
        stagingTex.Reset();
        sysMemSurf.Reset();
        m_stagingWidth  = srcDesc.Width;
        m_stagingHeight = srcDesc.Height;
        m_stagingFormat = srcDesc.Format;

        D3D11_TEXTURE2D_DESC st{};
        st.Width          = srcDesc.Width;
        st.Height         = srcDesc.Height;
        st.MipLevels      = 1;
        st.ArraySize      = 1;
        st.Format         = srcDesc.Format;
        st.SampleDesc     = {1, 0};
        st.Usage          = D3D11_USAGE_STAGING;
        st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        if (FAILED(d3d11Dev->CreateTexture2D(&st, nullptr, &stagingTex))) {
            LOG_ERROR("Failed to create D3D11 staging texture");
            return false;
        }
        LOG_VERBOSE("Staging texture (re)created: %ux%u fmt=%u",
                    m_stagingWidth, m_stagingHeight, (unsigned)m_stagingFormat);
    }

    if (!sysMemSurf) {
        HRESULT h = m_device->CreateOffscreenPlainSurface(
            m_stagingWidth, m_stagingHeight,
            D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM,
            &sysMemSurf, nullptr);
        if (FAILED(h)) {
            LOG_ERROR("CreateOffscreenPlainSurface (sysmem) failed: 0x%08X", h);
            return false;
        }
    }

    ComPtr<ID3D11DeviceContext> ctx;
    d3d11Dev->GetImmediateContext(&ctx);
    LOG_TRACE("Blit: CopyResource fmt=%u %ux%u", (unsigned)m_stagingFormat, m_stagingWidth, m_stagingHeight);
    ctx->CopyResource(stagingTex.Get(), srcTex.Get());

    LOG_TRACE("Blit: Map (blocking)...");
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        LOG_ERROR("D3D11 Map staging failed: 0x%08X", hr);
        return false;
    }
    LOG_TRACE("Blit: Map done rowPitch=%u", mapped.RowPitch);

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
    for (uint32_t row = 0; row < m_stagingHeight; ++row)
        memcpy(dstP + row * lr.Pitch, src + row * mapped.RowPitch, rowBytes);

    sysMemSurf->UnlockRect();
    ctx->Unmap(stagingTex.Get(), 0);

    hrd = m_device->UpdateSurface(sysMemSurf.Get(), nullptr, dst, nullptr);
    if (FAILED(hrd)) {
        LOG_ERROR("UpdateSurface failed: 0x%08X", hrd);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Stereo parameter control
// ---------------------------------------------------------------------------
void NvapiStereoPresenter::SetSeparation(float pct) {
    m_separation = std::max(0.0f, std::min(pct, 100.0f));
    if (!m_stereoHandle) return;
    NvAPI_Stereo_SetSeparation(m_stereoHandle, m_separation);
    LOG_INFO("Separation: %.1f%%", m_separation);
}

void NvapiStereoPresenter::SetConvergence(float val) {
    m_convergence = std::max(0.0f, std::min(val, 25.0f));
    if (!m_stereoHandle) return;
    NvAPI_Stereo_SetConvergence(m_stereoHandle, m_convergence);
    LOG_INFO("Convergence: %.2f", m_convergence);
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
NvapiStereoPresenter::~NvapiStereoPresenter() {
    m_backBufferLeft.Reset();
    m_backBufferRight.Reset();
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

    m_msgStop = true;
    if (m_hwnd) PostMessageW(m_hwnd, WM_QUIT, 0, 0);
    if (m_msgThread.joinable()) m_msgThread.join();
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }

    NvAPI_Unload();
}

// ---------------------------------------------------------------------------
bool NvapiIsAvailable() {
    if (NvAPI_Initialize() != NVAPI_OK) return false;
    NvU8 en = 0;
    NvAPI_Stereo_IsEnabled(&en);
    NvAPI_Unload();
    return en != 0;
}

} // namespace xr3dv
