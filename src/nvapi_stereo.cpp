//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  Stereo presentation: FULLSCREEN-EXCLUSIVE D3D9Ex + NV Packed-Stereo Surface.
//
//  Packed-surface technique:
//    Write left eye into rows [0,H), right into [H,2H), NVSTEREOIMAGEHEADER
//    into row [2H] of a W x (2H+1) surface.  StretchRect to W x H back buffer.
//    The NVIDIA DDI hook detects the signature and routes to stereo planes.
//    NvAPI_Stereo_IsActivated returning NO is expected on 3DFM-patched drivers;
//    the DDI hook still fires regardless of the IsActivated flag.
//
//  Focus / audio:
//    1. Subclass game WndProc BEFORE FSE creation so the deactivation cascade
//       caused by FSE (WM_ACTIVATE INACTIVE, WM_ACTIVATEAPP FALSE, WM_KILLFOCUS)
//       is swallowed before the engine sees it.
//    2. Signal m_initDone BEFORE RestoreFocusToGame to avoid deadlocking with
//       game threads that are blocked in xrCreateSession waiting for init.
//    3. Defer focus restore and activation retry via WM_APP messages so they
//       run on our own pump, never on the game thread.

#include "pch.h"
#include "nvapi_stereo.h"
#include "config.h"
#include "logging.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace xr3dv {

static constexpr UINT WM_XR3DV_RESTORE_FOCUS  = WM_APP + 1;
static constexpr UINT WM_XR3DV_RETRY_ACTIVATE = WM_APP + 2;

// ---------------------------------------------------------------------------
// Game-window subclass: swallows deactivation messages caused by FSE
// ---------------------------------------------------------------------------
static WNDPROC g_origGameWndProc    = nullptr;
static HWND    g_gameHwndSubclassed = nullptr;

static LRESULT CALLBACK GameWndSubclassProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            LOG_VERBOSE("Subclass: swallowed WM_ACTIVATE(INACTIVE)");
            return 0;
        }
        break;
    case WM_ACTIVATEAPP:
        if (!wp) {
            LOG_VERBOSE("Subclass: swallowed WM_ACTIVATEAPP(FALSE)");
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        LOG_VERBOSE("Subclass: swallowed WM_KILLFOCUS");
        return 0;
    }
    return CallWindowProcW(g_origGameWndProc, hw, msg, wp, lp);
}

static void InstallGameSubclass(HWND gameHwnd)
{
    if (!gameHwnd || g_origGameWndProc) return;
    g_gameHwndSubclassed = gameHwnd;
    g_origGameWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(gameHwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(GameWndSubclassProc)));
    if (g_origGameWndProc)
        LOG_INFO("Game window subclassed (WndProc=%p)", (void*)g_origGameWndProc);
    else
        LOG_ERROR("SetWindowLongPtrW failed GLE=%u — audio may cut on 3DV engage",
                  GetLastError());
}

static void RemoveGameSubclass()
{
    if (!g_gameHwndSubclassed || !g_origGameWndProc) return;
    SetWindowLongPtrW(g_gameHwndSubclassed, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(g_origGameWndProc));
    LOG_INFO("Game window WndProc restored");
    g_origGameWndProc    = nullptr;
    g_gameHwndSubclassed = nullptr;
}

// ---------------------------------------------------------------------------
// Game window search
// ---------------------------------------------------------------------------
struct FindGameWndCtx { DWORD pid; HWND result; HWND exclude; LONG bestArea; };

static BOOL CALLBACK FindGameWndEnum(HWND hw, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindGameWndCtx*>(lp);
    if (hw == ctx->exclude) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hw, &pid);
    if (pid != ctx->pid || !IsWindowVisible(hw)) return TRUE;
    RECT r{}; GetWindowRect(hw, &r);
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > ctx->bestArea) { ctx->bestArea = area; ctx->result = hw; }
    return TRUE;
}

static HWND FindGameWindow(HWND excludeHwnd)
{
    FindGameWndCtx ctx{ GetCurrentProcessId(), nullptr, excludeHwnd, 0 };
    EnumWindows(FindGameWndEnum, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

// ---------------------------------------------------------------------------
// FSE window procedure: forwards input to game
// ---------------------------------------------------------------------------
static HWND g_gameHwndForInput = nullptr;

static LRESULT CALLBACK XR3DVWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND gh = g_gameHwndForInput;
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:  case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:  case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:  case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
        if (gh) PostMessageW(gh, msg, wp, lp);
        return 0;
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_CHAR:    case WM_SYSCHAR:
        if (gh) PostMessageW(gh, msg, wp, lp);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Deferred focus restore (always called from our own message pump)
// ---------------------------------------------------------------------------
static void RestoreFocusToGame(HWND gameHwnd)
{
    if (!gameHwnd) return;
    DWORD ourThread  = GetCurrentThreadId();
    DWORD gameThread = GetWindowThreadProcessId(gameHwnd, nullptr);
    if (gameThread && gameThread != ourThread) {
        AttachThreadInput(ourThread, gameThread, TRUE);
        SetForegroundWindow(gameHwnd);
        SetActiveWindow(gameHwnd);
        SetFocus(gameHwnd);
        AttachThreadInput(ourThread, gameThread, FALSE);
    } else {
        SetForegroundWindow(gameHwnd);
        SetFocus(gameHwnd);
    }
    LOG_INFO("Focus restored to game window %p", (void*)gameHwnd);
}

// ---------------------------------------------------------------------------
// MsgThreadProc
// ---------------------------------------------------------------------------
void NvapiStereoPresenter::MsgThreadProc()
{
    // ---- 1. Find game window BEFORE our window exists ----
    // EnumWindows is reliable even if something else temporarily has focus.
    m_gameHwnd = FindGameWindow(nullptr);
    if (!m_gameHwnd) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) m_gameHwnd = fg;
    }
    // Subclass NOW so deactivation from FSE creation is swallowed
    if (m_gameHwnd) {
        InstallGameSubclass(m_gameHwnd);
        g_gameHwndForInput = m_gameHwnd;
    }
    LOG_INFO("Pre-FSE game window: %p (subclassed=%s)",
             (void*)m_gameHwnd, g_origGameWndProc ? "YES" : "NO");

    // ---- 2. Create window ----
    static const wchar_t kClass[] = L"XR3DV_D3D9";
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc); wc.lpfnWndProc = XR3DVWndProc;
        wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
    }
    m_hwnd = CreateWindowExW(0, kClass, L"XR3DV", WS_POPUP | WS_VISIBLE,
                              0, 0, (int)m_width, (int)m_height,
                              nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_hwnd) {
        LOG_ERROR("CreateWindowEx failed GLE=%u", GetLastError());
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, (int)m_width, (int)m_height,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    BringWindowToTop(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);

    {
        MSG tmp;
        for (int i = 0; i < 5; ++i) {
            Sleep(20);
            while (PeekMessageW(&tmp, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&tmp); DispatchMessageW(&tmp);
            }
        }
    }
    LOG_INFO("D3D9 window HWND=%p foreground=%s active=%s",
             (void*)m_hwnd,
             GetForegroundWindow() == m_hwnd ? "YES" : "NO",
             GetActiveWindow()     == m_hwnd ? "YES" : "NO");

    // ---- 3. D3D9Ex ----
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &m_d3d9);
    if (FAILED(hr)) {
        LOG_ERROR("Direct3DCreate9Ex failed: 0x%08X", hr);
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }

    {
        bool found = false;
        UINT cnt = m_d3d9->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
        for (UINT i = 0; i < cnt; ++i) {
            D3DDISPLAYMODE m{};
            m_d3d9->EnumAdapterModes(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8, i, &m);
            if (m.Width == m_width && m.Height == m_height && m.RefreshRate == m_fseRate)
                { found = true; break; }
        }
        if (!found)
            LOG_ERROR("No adapter mode %ux%u@%uHz — check MonitorRate= in xr3dv.ini",
                      m_width, m_height, m_fseRate);
    }

    // ---- 4. FSE device ----
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
    fsMode.Size = sizeof(D3DDISPLAYMODEEX);
    fsMode.Width = m_width; fsMode.Height = m_height;
    fsMode.RefreshRate = m_fseRate; fsMode.Format = D3DFMT_X8R8G8B8;
    fsMode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;

    hr = m_d3d9->CreateDeviceEx(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, m_hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
        &pp, &fsMode, &m_device);
    if (FAILED(hr)) {
        LOG_ERROR("CreateDeviceEx (FSE %ux%u@%uHz) failed: 0x%08X",
                  m_width, m_height, m_fseRate, hr);
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }
    LOG_INFO("D3D9 FSE device: %ux%u@%uHz", m_width, m_height, m_fseRate);

    // ---- 5. NVAPI stereo handle ----
    NvAPI_Status nvs = NvAPI_Stereo_CreateHandleFromIUnknown(m_device.Get(), &m_stereoHandle);
    if (nvs != NVAPI_OK) {
        LOG_ERROR("NvAPI_Stereo_CreateHandleFromIUnknown failed (%d)", nvs);
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }
    nvs = NvAPI_Stereo_Activate(m_stereoHandle);
    {
        NvU8 active = 0;
        NvAPI_Stereo_IsActivated(m_stereoHandle, &active);
        LOG_INFO("NVAPI stereo: Activate() code=%d  IsActivated=%s",
                 (int)nvs, active ? "YES" : "NO (retry scheduled)");
    }

    // ---- 6. Packed stereo surfaces: W x (H*2+1) ----
    const UINT ph = m_height * 2 + 1;
    hr = m_device->CreateOffscreenPlainSurface(
        m_width, ph, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &m_packedSysMem, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("CreateOffscreenPlainSurface SYSMEM %ux%u failed: 0x%08X", m_width, ph, hr);
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }
    hr = m_device->CreateOffscreenPlainSurface(
        m_width, ph, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &m_packedDefault, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("CreateOffscreenPlainSurface DEFAULT %ux%u failed: 0x%08X", m_width, ph, hr);
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }
    hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_backBuffer);
    if (FAILED(hr)) {
        LOG_ERROR("GetBackBuffer failed: 0x%08X", hr);
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }
    LOG_INFO("Packed stereo surfaces: %ux%u (each eye %ux%u)", m_width, ph, m_width, m_height);

    // ---- 7. Refresh game window post-FSE (may have changed) ----
    {
        HWND found = FindGameWindow(m_hwnd);
        if (found && found != m_gameHwnd) {
            LOG_INFO("Game window updated: %p -> %p", (void*)m_gameHwnd, (void*)found);
            RemoveGameSubclass();
            m_gameHwnd = found; g_gameHwndForInput = found;
            InstallGameSubclass(found);
        } else if (!m_gameHwnd && found) {
            m_gameHwnd = found; g_gameHwndForInput = found;
            InstallGameSubclass(found);
        }
    }
    if (m_gameHwnd) LOG_INFO("Game window: %p", (void*)m_gameHwnd);

    // ---- 8. Timers ----
    SetTimer(nullptr, TIMER_INPUT_POLL,  50,  nullptr);
    SetTimer(nullptr, TIMER_STEREO_SYNC, 500, nullptr);

    // ---- 9. Signal init complete BEFORE touching game thread ----
    // CRITICAL: game may be blocked in xrCreateSession waiting for m_initDone.
    // AttachThreadInput on a blocked thread = deadlock.
    // We post deferred messages so the game unblocks first.
    m_initOk.store(true);
    m_initDone.store(true);

    PostMessageW(m_hwnd, WM_XR3DV_RESTORE_FOCUS,  0, 0);
    PostMessageW(m_hwnd, WM_XR3DV_RETRY_ACTIVATE, 0, 0);

    // ---- Message pump ----
    while (!m_msgStop.load(std::memory_order_relaxed)) {
        MSG msg;
        if (MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT,
                                         MWMO_INPUTAVAILABLE) != WAIT_TIMEOUT) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { m_msgStop = true; goto done; }

                if (msg.message == WM_XR3DV_RESTORE_FOCUS) {
                    Sleep(150); // let xrCreateSession fully return on game thread
                    RestoreFocusToGame(m_gameHwnd);
                    continue;
                }

                if (msg.message == WM_XR3DV_RETRY_ACTIVATE) {
                    // Present a dummy black frame to prime the driver stereo
                    // state machine, then retry NvAPI_Stereo_Activate.
                    if (m_device && m_stereoHandle) {
                        m_device->Clear(0, nullptr, D3DCLEAR_TARGET,
                                        D3DCOLOR_XRGB(0,0,0), 1.f, 0);
                        m_device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
                        Sleep(50);
                        NvAPI_Stereo_Activate(m_stereoHandle);
                        NvU8 active = 0;
                        NvAPI_Stereo_IsActivated(m_stereoHandle, &active);
                        LOG_INFO("Stereo activation retry (post dummy Present): IsActivated=%s",
                                 active ? "YES" : "NO");
                    }
                    continue;
                }

                if (msg.message == WM_TIMER) {
                    if (msg.wParam == TIMER_INPUT_POLL) {
                        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                        if (ctrl) {
                            float sep = m_separation, conv = m_convergence;
                            bool  chg = false;
                            if      (GetAsyncKeyState(VK_F3) & 0x8000)
                                { sep  = std::max(0.0f,   sep  - kSepStep);  chg = true; }
                            else if (GetAsyncKeyState(VK_F4) & 0x8000)
                                { sep  = std::min(100.0f, sep  + kSepStep);  chg = true; }
                            if      (GetAsyncKeyState(VK_F5) & 0x8000)
                                { conv = std::max(0.0f,   conv - kConvStep); chg = true; }
                            else if (GetAsyncKeyState(VK_F6) & 0x8000)
                                { conv = std::min(25.0f,  conv + kConvStep); chg = true; }
                            if (GetAsyncKeyState(VK_F7) & 0x8000)
                                SaveGameStereoSettings(m_gameIniPath,
                                                       m_separation, m_convergence);
                            if (chg) { SetSeparation(sep); SetConvergence(conv); }
                        }
                    }
                    else if (msg.wParam == TIMER_STEREO_SYNC && m_stereoHandle) {
                        // Retry activation periodically (3DFM patched drivers
                        // may activate lazily), then sync OSD values.
                        NvU8 active = 0;
                        NvAPI_Stereo_IsActivated(m_stereoHandle, &active);
                        if (!active) {
                            NvAPI_Stereo_Activate(m_stereoHandle);
                            NvAPI_Stereo_IsActivated(m_stereoHandle, &active);
                            if (active) LOG_INFO("Stereo activated (deferred)");
                        }
                        if (active) {
                            float ds = 0.f, dc = 0.f;
                            NvAPI_Stereo_GetSeparation(m_stereoHandle, &ds);
                            NvAPI_Stereo_GetConvergence(m_stereoHandle, &dc);
                            if (fabsf(ds - m_separation)  > 0.05f) {
                                m_separation = ds;
                                LOG_INFO("Separation synced from 3DV: %.1f%%", m_separation);
                            }
                            if (fabsf(dc - m_convergence) > 0.005f) {
                                m_convergence = dc;
                                LOG_INFO("Convergence synced from 3DV: %.3f", m_convergence);
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
    KillTimer(nullptr, TIMER_STEREO_SYNC);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::Init(uint32_t width, uint32_t height,
                                 uint32_t fseRate,
                                 float separation, float convergence,
                                 bool swapEyes,
                                 const std::string& gameIniPath)
{
    m_width = width; m_height = height; m_fseRate = fseRate;
    m_separation = separation; m_convergence = convergence;
    m_swapEyes = swapEyes; m_gameIniPath = gameIniPath;

    NvAPI_Status nvs = NvAPI_Initialize();
    if (nvs != NVAPI_OK) {
        NvAPI_ShortString err; NvAPI_GetErrorMessage(nvs, err);
        LOG_ERROR("NvAPI_Initialize: %s", err); return false;
    }
    nvs = NvAPI_Stereo_Enable();
    if (nvs != NVAPI_OK)
        LOG_ERROR("NvAPI_Stereo_Enable failed (%d)", nvs);

    m_msgStop = false; m_initDone = false; m_initOk = false;
    m_msgThread = std::thread([this]() { MsgThreadProc(); });

    // Wait up to 10 s — ATS can be slow during initial load
    for (int i = 0; i < 1000 && !m_initDone.load(); ++i) Sleep(10);
    if (!m_initDone.load()) {
        LOG_ERROR("Timed out waiting for D3D9 init (10s)");
        m_msgStop = true;
        if (m_msgThread.joinable()) m_msgThread.join();
        return false;
    }
    if (!m_initOk.load()) {
        LOG_ERROR("D3D9 init failed");
        m_msgStop = true;
        if (m_msgThread.joinable()) m_msgThread.join();
        return false;
    }

    SetSeparation(separation);
    SetConvergence(convergence);
    m_initialised = true;
    LOG_INFO("NvapiStereoPresenter ready (%ux%u FSE @%uHz, sep=%.1f%%, conv=%.2f)",
             width, height, fseRate, separation, convergence);
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

    if (!BlitD3D11ToPacked(leftSRV,  d3d11Dev, 0,        m_stagingLeft))  return false;
    if (!BlitD3D11ToPacked(rightSRV, d3d11Dev, m_height, m_stagingRight)) return false;

    {
        D3DLOCKED_RECT lr{};
        HRESULT h = m_packedSysMem->LockRect(&lr, nullptr, 0);
        if (FAILED(h)) { LOG_ERROR("LockRect (header) failed: 0x%08X", h); return false; }
        auto* hdr = reinterpret_cast<NvStereoImageHeader*>(
            static_cast<uint8_t*>(lr.pBits) + (size_t)2 * m_height * lr.Pitch);
        hdr->signature = NVSTEREO_IMAGE_SIGNATURE;
        hdr->width     = m_width;
        hdr->height    = m_height;
        hdr->bpp       = 32;
        hdr->flags     = m_swapEyes ? 1u : 0u;
        m_packedSysMem->UnlockRect();
    }

    HRESULT h = m_device->UpdateSurface(
        m_packedSysMem.Get(), nullptr, m_packedDefault.Get(), nullptr);
    if (FAILED(h)) { LOG_ERROR("UpdateSurface failed: 0x%08X", h); return false; }

    RECT src{ 0, 0, (LONG)m_width, (LONG)(m_height * 2 + 1) };
    RECT dst{ 0, 0, (LONG)m_width, (LONG)m_height };
    h = m_device->StretchRect(
        m_packedDefault.Get(), &src, m_backBuffer.Get(), &dst, D3DTEXF_POINT);
    if (FAILED(h)) { LOG_ERROR("StretchRect failed: 0x%08X", h); return false; }

    h = m_device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    if (FAILED(h)) { LOG_ERROR("PresentEx failed: 0x%08X", h); return false; }

    LOG_TRACE("PresentStereoFrame: done");
    return true;
}

// ---------------------------------------------------------------------------
// BlitD3D11ToPacked
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::BlitD3D11ToPacked(
    ID3D11ShaderResourceView*   srv,
    ID3D11Device*               d3d11Dev,
    uint32_t                    yOffset,
    ComPtr<ID3D11Texture2D>&    stagingTex)
{
    ComPtr<ID3D11Resource> res;
    srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> srcTex;
    if (FAILED(res.As(&srcTex))) {
        LOG_ERROR("BlitD3D11ToPacked: SRV is not Texture2D"); return false;
    }
    D3D11_TEXTURE2D_DESC sd{};
    srcTex->GetDesc(&sd);

    if (!stagingTex || m_stagingWidth != sd.Width ||
        m_stagingHeight != sd.Height || m_stagingFormat != sd.Format) {
        stagingTex.Reset();
        m_stagingWidth = sd.Width; m_stagingHeight = sd.Height; m_stagingFormat = sd.Format;
        D3D11_TEXTURE2D_DESC st{};
        st.Width = sd.Width; st.Height = sd.Height;
        st.MipLevels = 1; st.ArraySize = 1; st.Format = sd.Format;
        st.SampleDesc = {1,0}; st.Usage = D3D11_USAGE_STAGING;
        st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(d3d11Dev->CreateTexture2D(&st, nullptr, &stagingTex))) {
            LOG_ERROR("CreateTexture2D (staging) failed"); return false;
        }
        LOG_VERBOSE("Staging texture (re)created: %ux%u fmt=%u (yOffset=%u)",
                    m_stagingWidth, m_stagingHeight, (unsigned)m_stagingFormat, yOffset);
    }

    ComPtr<ID3D11DeviceContext> ctx;
    d3d11Dev->GetImmediateContext(&ctx);
    ctx->CopyResource(stagingTex.Get(), srcTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) { LOG_ERROR("D3D11 Map failed: 0x%08X", hr); return false; }

    D3DLOCKED_RECT lr{};
    hr = m_packedSysMem->LockRect(&lr, nullptr, 0);
    if (FAILED(hr)) {
        ctx->Unmap(stagingTex.Get(), 0);
        LOG_ERROR("LockRect (SYSMEM) failed: 0x%08X", hr); return false;
    }

    const uint8_t* src  = static_cast<const uint8_t*>(mapped.pData);
    uint8_t*       dst  = static_cast<uint8_t*>(lr.pBits) + (size_t)yOffset * lr.Pitch;
    const size_t   rowB = (size_t)sd.Width * 4;
    for (uint32_t row = 0; row < sd.Height; ++row)
        memcpy(dst + (size_t)row * lr.Pitch,
               src + (size_t)row * mapped.RowPitch, rowB);

    m_packedSysMem->UnlockRect();
    ctx->Unmap(stagingTex.Get(), 0);
    return true;
}

// ---------------------------------------------------------------------------
void NvapiStereoPresenter::SetSeparation(float pct) {
    m_separation = std::max(0.0f, std::min(pct, 100.0f));
    if (m_stereoHandle) NvAPI_Stereo_SetSeparation(m_stereoHandle, m_separation);
    LOG_INFO("Separation: %.1f%%", m_separation);
}
void NvapiStereoPresenter::SetConvergence(float val) {
    m_convergence = std::max(0.0f, std::min(val, 25.0f));
    if (m_stereoHandle) NvAPI_Stereo_SetConvergence(m_stereoHandle, m_convergence);
    LOG_INFO("Convergence: %.3f", m_convergence);
}

// ---------------------------------------------------------------------------
NvapiStereoPresenter::~NvapiStereoPresenter() {
    m_initialised = false;
    m_backBuffer.Reset(); m_packedSysMem.Reset(); m_packedDefault.Reset();
    m_stagingLeft.Reset(); m_stagingRight.Reset();
    if (m_stereoHandle) {
        NvAPI_Stereo_DestroyHandle(m_stereoHandle);
        m_stereoHandle = nullptr;
    }
    m_device.Reset(); m_d3d9.Reset();
    RemoveGameSubclass();
    g_gameHwndForInput = nullptr;
    m_msgStop = true;
    if (m_hwnd) PostMessageW(m_hwnd, WM_QUIT, 0, 0);
    if (m_msgThread.joinable()) m_msgThread.join();
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    NvAPI_Unload();
}

// ---------------------------------------------------------------------------
bool NvapiIsAvailable() {
    if (NvAPI_Initialize() != NVAPI_OK) return false;
    NvU8 en = 0; NvAPI_Stereo_IsEnabled(&en);
    NvAPI_Unload();
    return en != 0;
}

} // namespace xr3dv
