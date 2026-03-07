//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  NVAPI stereo bridge: creates a fullscreen-exclusive D3D9Ex device,
//  enables NVAPI stereo, and blits D3D11 left/right eye textures each frame.
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
// Find the game's main window (largest visible top-level window in our
// process that isn't the XR3DV D3D9 window).
// ---------------------------------------------------------------------------
struct FindGameWndCtx { DWORD pid; HWND result; HWND exclude; LONG bestArea; };

static BOOL CALLBACK FindGameWndProc(HWND hw, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindGameWndCtx*>(lp);
    if (hw == ctx->exclude) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hw, &pid);
    if (pid != ctx->pid) return TRUE;
    if (!IsWindowVisible(hw)) return TRUE;
    RECT r{};
    GetWindowRect(hw, &r);
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > ctx->bestArea) {
        ctx->bestArea = area;
        ctx->result   = hw;
    }
    return TRUE;
}

static HWND FindGameWindow(HWND excludeHwnd) {
    FindGameWndCtx ctx{ GetCurrentProcessId(), nullptr, excludeHwnd, 0 };
    EnumWindows(FindGameWndProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

// ---------------------------------------------------------------------------
// Custom window procedure — forwards mouse/keyboard to game window so clicks
// pass through the FSE overlay to the game's UI.
// ---------------------------------------------------------------------------
struct WndProcCtx { HWND gameHwnd; };
static WndProcCtx g_wpCtx{};

static LRESULT CALLBACK XR3DVWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    HWND gh = g_wpCtx.gameHwnd;
    switch (msg) {
    // Forward all mouse input to the game window
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:   case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:   case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:   case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
        if (gh) PostMessageW(gh, msg, wp, lp);
        return 0;

    // Forward keyboard input
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_CHAR:    case WM_SYSCHAR:
        if (gh) PostMessageW(gh, msg, wp, lp);
        return 0;

    // Don't let our window actually activate (steal focus) via clicks
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Create presentation window
// ---------------------------------------------------------------------------
static HWND CreatePresentationWindow() {
    static const wchar_t kClass[] = L"XR3DV_D3D9";
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = XR3DVWndProc;   // custom proc for input forwarding
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.style         = CS_NOCLOSE;
    RegisterClassExW(&wc);

    HWND hw = CreateWindowExW(
        WS_EX_NOACTIVATE,   // don't steal activation on creation
        kClass, L"XR3DV", WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, wc.hInstance, nullptr);
    return hw;
}

// ---------------------------------------------------------------------------
// Message + D3D9 thread — ALL D3D9 FSE work AND message pump live here.
// ---------------------------------------------------------------------------
void NvapiStereoPresenter::MsgThreadProc() {
    // ---- Save the game's foreground window before we steal it ----
    m_gameHwnd = GetForegroundWindow();

    // ---- Create window, then activate it for FSE ----
    m_hwnd = CreatePresentationWindow();
    if (!m_hwnd) {
        LOG_ERROR("CreatePresentationWindow failed (GLE=%u)", GetLastError());
        m_initOk.store(false); m_initDone.store(true); return;
    }

    // Install global context for the WndProc
    g_wpCtx.gameHwnd = m_gameHwnd;

    ShowWindow(m_hwnd, SW_SHOW);
    BringWindowToTop(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);

    // Brief pump to let window reach foreground before D3D9 init
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

    // ---- Create D3D9Ex ----
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &m_d3d9);
    if (FAILED(hr)) {
        LOG_ERROR("Direct3DCreate9Ex failed: 0x%08X", hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }

    // ---- Enumerate adapter modes ----
    {
        UINT cnt = m_d3d9->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
        LOG_INFO("Adapter modes (X8R8G8B8): %u", cnt);
        bool found = false;
        for (UINT i = 0; i < cnt; ++i) {
            D3DDISPLAYMODE m{};
            m_d3d9->EnumAdapterModes(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8, i, &m);
            bool match = (m.Width == m_width && m.Height == m_height && m.RefreshRate == m_fseRate);
            LOG_VERBOSE("  [%u] %ux%u@%uHz%s", i, m.Width, m.Height, m.RefreshRate,
                        match ? " <-- TARGET" : "");
            if (match) found = true;
        }
        if (!found)
            LOG_ERROR("No adapter mode matching %ux%u@%uHz -- check MonitorRate= in xr3dv.ini",
                      m_width, m_height, m_fseRate);
    }

    // ---- Create FSE device ----
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
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
        &pp, &fsMode, &m_device);

    if (FAILED(hr)) {
        LOG_ERROR("CreateDeviceEx (FSE %ux%u@%uHz) failed: 0x%08X",
                  m_width, m_height, m_fseRate, hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }
    LOG_INFO("D3D9 FSE device: %ux%u@%uHz", m_width, m_height, m_fseRate);

    // ---- NVAPI stereo handle + activate ----
    NvAPI_Status nvs = NvAPI_Stereo_CreateHandleFromIUnknown(m_device.Get(), &m_stereoHandle);
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
        NvU8 active = 0;
        NvAPI_Stereo_IsActivated(m_stereoHandle, &active);
        LOG_INFO("NVAPI stereo activated: %s", active ? "YES" : "NO -- check NVIDIA Control Panel");
    }

    // ---- Per-eye offscreen surfaces (plain mono DEFAULT pool surfaces) ----
    // Source surfaces for StretchRect are just normal mono surfaces.
    // SetActiveEye routes writes to the mono backbuffer → appropriate stereo plane.
    hr = m_device->CreateOffscreenPlainSurface(
        m_width, m_height, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &m_leftSurface, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("CreateOffscreenPlainSurface (left) failed: 0x%08X", hr); 
        m_initOk.store(false); m_initDone.store(true); return;
    }
    hr = m_device->CreateOffscreenPlainSurface(
        m_width, m_height, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &m_rightSurface, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("CreateOffscreenPlainSurface (right) failed: 0x%08X", hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }

    // ---- Get mono back buffer — SetActiveEye redirects StretchRect to the correct plane ----
    hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_backBuffer);
    if (FAILED(hr)) {
        LOG_ERROR("GetBackBuffer failed: 0x%08X", hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }

    // ---- Now find the game's actual main window (which may differ after FSE took over) ----
    HWND gameWnd = FindGameWindow(m_hwnd);
    if (gameWnd) {
        m_gameHwnd       = gameWnd;
        g_wpCtx.gameHwnd = gameWnd;
        LOG_INFO("Game main window: %p", (void*)gameWnd);
    }

    // ---- Restore audio: post fake activate/focus to game window ----
    if (m_gameHwnd) {
        PostMessageW(m_gameHwnd, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), (LPARAM)m_hwnd);
        PostMessageW(m_gameHwnd, WM_SETFOCUS, (WPARAM)m_hwnd, 0);
    }

    // ---- Start timers ----
    // TIMER_INPUT_POLL  — 50 ms for hotkey-hold detection via GetAsyncKeyState
    // TIMER_AUDIO_KEEP  — 2000 ms to keep game audio alive while we have FSE
    // TIMER_STEREO_SYNC — 500 ms to sync sep/conv with 3DV OSD (bidirectional)
    SetTimer(nullptr, TIMER_INPUT_POLL,  50,   nullptr);
    SetTimer(nullptr, TIMER_AUDIO_KEEP,  2000, nullptr);
    SetTimer(nullptr, TIMER_STEREO_SYNC, 500,  nullptr);

    // Signal Init() that setup is complete
    m_initOk.store(true);
    m_initDone.store(true);

    // ---- Message pump ----
    while (!m_msgStop.load(std::memory_order_relaxed)) {
        MSG msg;
        if (MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT,
                                         MWMO_INPUTAVAILABLE) != WAIT_TIMEOUT) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { m_msgStop = true; goto done; }

                if (msg.message == WM_TIMER) {
                    if (msg.wParam == TIMER_INPUT_POLL) {
                        // --- Hotkey-hold polling ---
                        // All keys are checked every 50 ms so holding adjusts continuously.
                        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                        if (ctrl) {
                            float sep  = m_separation;
                            float conv = m_convergence;
                            bool changed = false;
                            if (GetAsyncKeyState(VK_F3) & 0x8000) {
                                sep = std::max(0.0f,   sep  - kSepStep);  changed = true;
                            } else if (GetAsyncKeyState(VK_F4) & 0x8000) {
                                sep = std::min(100.0f, sep  + kSepStep);  changed = true;
                            }
                            if (GetAsyncKeyState(VK_F5) & 0x8000) {
                                conv = std::max(0.0f,  conv - kConvStep); changed = true;
                            } else if (GetAsyncKeyState(VK_F6) & 0x8000) {
                                conv = std::min(25.0f, conv + kConvStep); changed = true;
                            }
                            if (GetAsyncKeyState(VK_F7) & 0x8000) {
                                SaveGameStereoSettings(m_gameIniPath, m_separation, m_convergence);
                            }
                            if (changed) {
                                SetSeparation(sep);
                                SetConvergence(conv);
                            }
                        }
                    }
                    else if (msg.wParam == TIMER_AUDIO_KEEP) {
                        // Keep game audio alive while we hold FSE.
                        // Many engines silence audio when WM_ACTIVATE(INACTIVE) is received.
                        // Periodically re-send WA_ACTIVE to counteract this.
                        if (m_gameHwnd) {
                            PostMessageW(m_gameHwnd, WM_ACTIVATE,
                                         MAKEWPARAM(WA_ACTIVE, 0), (LPARAM)m_hwnd);
                        }
                    }
                    else if (msg.wParam == TIMER_STEREO_SYNC) {
                        // Bidirectionally sync sep/conv with the 3DV driver.
                        // This means: if the user adjusts via the 3DV hardware hotkeys
                        // (or the 3DV emitter buttons), our values track the OSD values.
                        if (m_stereoHandle) {
                            float drvSep = 0.0f, drvConv = 0.0f;
                            NvAPI_Stereo_GetSeparation(m_stereoHandle, &drvSep);
                            NvAPI_Stereo_GetConvergence(m_stereoHandle, &drvConv);
                            // Only update if the driver value differs noticeably
                            // (driver is authoritative — user may have used 3DV hotkeys)
                            if (fabsf(drvSep  - m_separation)  > 0.05f) {
                                m_separation  = drvSep;
                                LOG_INFO("Separation synced from 3DV OSD: %.1f%%", m_separation);
                            }
                            if (fabsf(drvConv - m_convergence) > 0.005f) {
                                m_convergence = drvConv;
                                LOG_INFO("Convergence synced from 3DV OSD: %.3f", m_convergence);
                            }
                        }
                    }
                }

                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
done:
    KillTimer(nullptr, TIMER_INPUT_POLL);
    KillTimer(nullptr, TIMER_AUDIO_KEEP);
    KillTimer(nullptr, TIMER_STEREO_SYNC);
}

// ---------------------------------------------------------------------------
// NvapiStereoPresenter::Init
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::Init(uint32_t width, uint32_t height,
                                 uint32_t fseRate,
                                 float separation, float convergence,
                                 const std::string& gameIniPath)
{
    m_width       = width;
    m_height      = height;
    m_fseRate     = fseRate;
    m_separation  = separation;
    m_convergence = convergence;
    m_gameIniPath = gameIniPath;

    // 1. Initialise NVAPI on calling thread
    NvAPI_Status nvs = NvAPI_Initialize();
    if (nvs != NVAPI_OK) {
        NvAPI_ShortString errStr;
        NvAPI_GetErrorMessage(nvs, errStr);
        LOG_ERROR("NvAPI_Initialize failed: %s", errStr);
        return false;
    }
    nvs = NvAPI_Stereo_Enable();
    if (nvs != NVAPI_OK) {
        LOG_ERROR("NvAPI_Stereo_Enable failed (%d) -- ensure 3D Vision is on in NVIDIA Control Panel", nvs);
        return false;
    }

    // 2. Spin up message thread (ALL D3D9 init happens inside MsgThreadProc)
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

    // 3. Push initial stereo parameters into the driver
    SetSeparation(separation);
    SetConvergence(convergence);

    m_initialised = true;
    LOG_INFO("NvapiStereoPresenter initialised (%ux%u@%uHz FSE, sep=%.1f%%, conv=%.2f)",
             width, height, fseRate, separation, convergence);
    return true;
}

// ---------------------------------------------------------------------------
// Stubs (all real work moved to MsgThreadProc)
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
    if (!BlitD3D11ToD3D9(leftSRV, d3d11Dev, m_leftSurface.Get(),
                          m_stagingTex, m_sysMemLeft)) return false;

    // SetActiveEye(LEFT) → driver routes the following StretchRect to the left stereo plane
    NvAPI_Stereo_SetActiveEye(m_stereoHandle, NVAPI_STEREO_EYE_LEFT);
    m_device->StretchRect(m_leftSurface.Get(), nullptr,
                          m_backBuffer.Get(), nullptr, D3DTEXF_LINEAR);

    // Blit right eye
    if (!BlitD3D11ToD3D9(rightSRV, d3d11Dev, m_rightSurface.Get(),
                          m_stagingTexRight, m_sysMemRight)) return false;

    // SetActiveEye(RIGHT) → routes StretchRect to right stereo plane
    NvAPI_Stereo_SetActiveEye(m_stereoHandle, NVAPI_STEREO_EYE_RIGHT);
    m_device->StretchRect(m_rightSurface.Get(), nullptr,
                          m_backBuffer.Get(), nullptr, D3DTEXF_LINEAR);

    // Reset to mono so unrelated D3D9 calls aren't accidentally routed
    NvAPI_Stereo_SetActiveEye(m_stereoHandle, NVAPI_STEREO_EYE_MONO);

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
            D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysMemSurf, nullptr);
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
    if (m_stereoHandle)
        NvAPI_Stereo_SetSeparation(m_stereoHandle, m_separation);
    LOG_INFO("Separation: %.1f%%", m_separation);
}

void NvapiStereoPresenter::SetConvergence(float val) {
    m_convergence = std::max(0.0f, std::min(val, 25.0f));
    if (m_stereoHandle)
        NvAPI_Stereo_SetConvergence(m_stereoHandle, m_convergence);
    LOG_INFO("Convergence: %.3f", m_convergence);
}

// ---------------------------------------------------------------------------
// Destructor
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
