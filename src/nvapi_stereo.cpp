//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  Stereo presentation strategy
//  ─────────────────────────────
//  Primary: NvAPI_Stereo_SetActiveEye + StretchRect to the mono backbuffer.
//    The NVIDIA driver routes writes to the left/right stereo planes at the
//    hardware level.  This works on modern drivers patched by 3D Fix Manager
//    because SetActiveEye goes through nvapi.dll (which 3DFM patches).
//
//  NOT used: Packed-surface StretchRect (NVSTEREO_IMAGE_SIGNATURE).
//    That technique relies on a DDI hook in nvlddmkm.sys which 3DFM does NOT
//    restore.  It appears to succeed (StretchRect returns S_OK) but the driver
//    treats it as a normal scale, giving top-and-bottom to both eyes.
//
//  Device: Fullscreen-exclusive D3D9Ex.
//    3D Vision only activates on FSE devices on drivers ≥ 426.06.
//    IsActivated() may return NO on 3DFM-patched drivers even when 3DV is
//    visually active — this is an API quirk; SetActiveEye still routes correctly.
//
//  Focus / audio: game WndProc subclassing.
//    Installed BEFORE FSE creation so the game's engine never sees the
//    WM_ACTIVATE(WA_INACTIVE) / WM_KILLFOCUS messages that FSE triggers.
//    m_initDone is signalled BEFORE focus is handed back to the game so that
//    a slow or hung RestoreFocus doesn't time out the calling thread.

#include "pch.h"
#include "nvapi_stereo.h"
#include "config.h"
#include "logging.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace xr3dv {

// ──────────────────────────────────────────────────────────────────────────
// Game-window WndProc subclass
// Intercepts deactivation messages BEFORE they reach the engine so audio and
// focus stay with the game even while our FSE device owns the screen.
// ──────────────────────────────────────────────────────────────────────────
static WNDPROC g_origGameWndProc    = nullptr;
static HWND    g_subclassedGameHwnd = nullptr;

static LRESULT CALLBACK GameWndSubclassProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) return 0;   // swallow: don't let engine know
        break;
    case WM_ACTIVATEAPP:
        if (wp == FALSE) return 0;
        break;
    case WM_KILLFOCUS:
        return 0;
    }
    return CallWindowProcW(g_origGameWndProc, hw, msg, wp, lp);
}

static void InstallGameSubclass(HWND gameHwnd)
{
    if (!gameHwnd || g_origGameWndProc) return;
    g_subclassedGameHwnd = gameHwnd;
    g_origGameWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(gameHwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(GameWndSubclassProc)));
    if (g_origGameWndProc)
        LOG_INFO("Game window subclassed (WndProc=%p)", (void*)g_origGameWndProc);
    else
        LOG_ERROR("SetWindowLongPtrW (subclass) GLE=%u", GetLastError());
}

static void RemoveGameSubclass()
{
    if (!g_subclassedGameHwnd || !g_origGameWndProc) return;
    SetWindowLongPtrW(g_subclassedGameHwnd, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(g_origGameWndProc));
    g_origGameWndProc    = nullptr;
    g_subclassedGameHwnd = nullptr;
    LOG_INFO("Game window WndProc restored");
}

// ──────────────────────────────────────────────────────────────────────────
// Enumerate visible top-level windows in our process to find the game's main
// window (largest area, not our own FSE window).
// ──────────────────────────────────────────────────────────────────────────
struct FindCtx { DWORD pid; HWND exclude; HWND result; LONG bestArea; };

static BOOL CALLBACK FindWindowCb(HWND hw, LPARAM lp)
{
    auto* c = reinterpret_cast<FindCtx*>(lp);
    if (hw == c->exclude) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(hw, &pid);
    if (pid != c->pid) return TRUE;
    if (!IsWindowVisible(hw)) return TRUE;
    RECT r{}; GetWindowRect(hw, &r);
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > c->bestArea) { c->bestArea = area; c->result = hw; }
    return TRUE;
}

static HWND FindGameWindow(HWND excludeHwnd)
{
    FindCtx c{ GetCurrentProcessId(), excludeHwnd, nullptr, 0 };
    EnumWindows(FindWindowCb, reinterpret_cast<LPARAM>(&c));
    return c.result;
}

// ──────────────────────────────────────────────────────────────────────────
// Restore focus to the game window WITHOUT AttachThreadInput (which can
// deadlock when the game thread is in its own FSE init).
// We use PostMessage so we never block on the game thread's message queue.
// ──────────────────────────────────────────────────────────────────────────
static void RestoreFocusToGame(HWND gameHwnd)
{
    if (!gameHwnd) return;
    // AllowSetForegroundWindow lets a process grant fg rights to another;
    // since we're in the same process, this is enough.
    AllowSetForegroundWindow(GetCurrentProcessId());
    // SwitchToThisWindow is the only reliable cross-thread fg-grant that
    // doesn't require AttachThreadInput.
    SwitchToThisWindow(gameHwnd, FALSE);
    // Belt-and-suspenders: post synthetic activate/focus so the engine's
    // message handlers see a proper activation even if SwitchToThisWindow
    // fails silently (which it may if Windows throttles fg changes).
    PostMessageW(gameHwnd, WM_ACTIVATE,    MAKEWPARAM(WA_ACTIVE, 0), (LPARAM)gameHwnd);
    PostMessageW(gameHwnd, WM_SETFOCUS,    0, 0);
    PostMessageW(gameHwnd, WM_ACTIVATEAPP, TRUE, (LPARAM)GetCurrentThreadId());
    LOG_INFO("Focus restored to game window %p", (void*)gameHwnd);
}

// ──────────────────────────────────────────────────────────────────────────
// FSE window procedure — forwards all input to the game window.
// ──────────────────────────────────────────────────────────────────────────
static HWND g_fwdGameHwnd = nullptr;

static LRESULT CALLBACK XR3DVWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND gh = g_fwdGameHwnd;
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

// ──────────────────────────────────────────────────────────────────────────
// MsgThreadProc — D3D9 init + message pump
// ──────────────────────────────────────────────────────────────────────────
void NvapiStereoPresenter::MsgThreadProc()
{
    // ── 1. Find game window and install subclass BEFORE taking FSE ──────
    // GetForegroundWindow while we're still in the game's thread context gives
    // us the game's window before FSE deactivation messages fly.
    HWND fgWnd = GetForegroundWindow();
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(fgWnd, &pid);
        if (pid != GetCurrentProcessId()) fgWnd = nullptr;
    }
    if (fgWnd) InstallGameSubclass(fgWnd);   // must be before CreateDeviceEx

    // ── 2. Create our FSE window ─────────────────────────────────────────
    static const wchar_t kClass[] = L"XR3DV_D3D9";
    {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = XR3DVWndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
    }

    m_hwnd = CreateWindowExW(0, kClass, L"XR3DV",
                              WS_POPUP, 0, 0, (int)m_width, (int)m_height,
                              nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_hwnd) {
        LOG_ERROR("CreateWindowEx failed (GLE=%u)", GetLastError());
        m_initOk.store(false); m_initDone.store(true); return;
    }

    // Bring to foreground aggressively — needed for FSE
    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    SetWindowPos(m_hwnd, HWND_TOP, 0, 0, (int)m_width, (int)m_height, SWP_SHOWWINDOW);
    BringWindowToTop(m_hwnd);
    AllowSetForegroundWindow(ASFW_ANY);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SwitchToThisWindow(m_hwnd, FALSE);
    SetFocus(m_hwnd);

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

    // ── 3. D3D9Ex ─────────────────────────────────────────────────────────
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &m_d3d9);
    if (FAILED(hr)) {
        LOG_ERROR("Direct3DCreate9Ex failed: 0x%08X", hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }

    // Validate mode
    {
        bool found = false;
        UINT n = m_d3d9->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
        for (UINT i = 0; i < n; ++i) {
            D3DDISPLAYMODE m{};
            m_d3d9->EnumAdapterModes(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8, i, &m);
            if (m.Width == m_width && m.Height == m_height && m.RefreshRate == m_fseRate)
                { found = true; break; }
        }
        if (!found)
            LOG_ERROR("No adapter mode %ux%u@%uHz — check MonitorRate= in xr3dv.ini",
                      m_width, m_height, m_fseRate);
    }

    // ── 4. Create FSE device ──────────────────────────────────────────────
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
        RemoveGameSubclass();
        m_initOk.store(false); m_initDone.store(true); return;
    }
    LOG_INFO("D3D9 FSE device: %ux%u@%uHz", m_width, m_height, m_fseRate);

    // ── 5. NVAPI stereo handle + activate ────────────────────────────────
    NvAPI_Status nvs = NvAPI_Stereo_CreateHandleFromIUnknown(m_device.Get(), &m_stereoHandle);
    if (nvs != NVAPI_OK) {
        LOG_ERROR("NvAPI_Stereo_CreateHandleFromIUnknown failed (%d)", nvs);
        RemoveGameSubclass();
        m_initOk.store(false); m_initDone.store(true); return;
    }
    nvs = NvAPI_Stereo_Activate(m_stereoHandle);
    if (nvs != NVAPI_OK)
        LOG_ERROR("NvAPI_Stereo_Activate returned %d (3DFM quirk — continuing)", nvs);
    {
        NvU8 active = 0;
        NvAPI_Stereo_IsActivated(m_stereoHandle, &active);
        LOG_INFO("NVAPI stereo IsActivated=%s (SetActiveEye path used regardless)",
                 active ? "YES" : "NO");
    }

    // ── 6. Per-eye DEFAULT pool surfaces ──────────────────────────────────
    // NvAPI_Stereo_SetActiveEye routes subsequent StretchRect calls to the
    // left or right stereo plane of the backbuffer.  Plain mono surfaces are
    // used as the source — no packed-surface magic needed.
    hr = m_device->CreateOffscreenPlainSurface(
        m_width, m_height, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &m_leftSurface, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("CreateOffscreenPlainSurface (left) failed: 0x%08X", hr);
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }
    hr = m_device->CreateOffscreenPlainSurface(
        m_width, m_height, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &m_rightSurface, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("CreateOffscreenPlainSurface (right) failed: 0x%08X", hr);
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }
    hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_backBuffer);
    if (FAILED(hr)) {
        LOG_ERROR("GetBackBuffer failed: 0x%08X", hr);
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }
    LOG_INFO("Per-eye surfaces created: %ux%u each", m_width, m_height);

    // ── 7. Find game window (may differ from fg window at startup) ────────
    m_gameHwnd = FindGameWindow(m_hwnd);
    if (!m_gameHwnd) m_gameHwnd = fgWnd;
    if (m_gameHwnd && m_gameHwnd != fgWnd)
        InstallGameSubclass(m_gameHwnd);   // subclass the real game window too
    if (m_gameHwnd) {
        g_fwdGameHwnd = m_gameHwnd;
        LOG_INFO("Game window: %p", (void*)m_gameHwnd);
    }

    // ── 8. Signal init DONE before handing focus back ─────────────────────
    // This ensures Init() on the calling thread doesn't time out even if
    // RestoreFocusToGame is slow (e.g. when ATS is also in FSE init).
    m_initOk.store(true);
    m_initDone.store(true);

    // Restore focus to game — PostMessage-only, no AttachThreadInput
    RestoreFocusToGame(m_gameHwnd);

    // ── 9. Timers ─────────────────────────────────────────────────────────
    SetTimer(nullptr, TIMER_INPUT_POLL,  50,  nullptr);
    SetTimer(nullptr, TIMER_STEREO_SYNC, 500, nullptr);

    // ── Message pump ──────────────────────────────────────────────────────
    while (!m_msgStop.load(std::memory_order_relaxed)) {
        MSG msg;
        if (MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT,
                                         MWMO_INPUTAVAILABLE) != WAIT_TIMEOUT) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { m_msgStop = true; goto done; }

                if (msg.message == WM_TIMER) {
                    if (msg.wParam == TIMER_INPUT_POLL) {
                        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                        if (ctrl) {
                            float sep  = m_separation, conv = m_convergence;
                            bool  chg  = false;
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
                        float dSep = 0.0f, dConv = 0.0f;
                        NvAPI_Stereo_GetSeparation(m_stereoHandle, &dSep);
                        NvAPI_Stereo_GetConvergence(m_stereoHandle, &dConv);
                        if (fabsf(dSep  - m_separation)  > 0.05f) {
                            m_separation = dSep;
                            LOG_INFO("Separation synced from 3DV OSD: %.1f%%", m_separation);
                        }
                        if (fabsf(dConv - m_convergence) > 0.005f) {
                            m_convergence = dConv;
                            LOG_INFO("Convergence synced from 3DV OSD: %.3f", m_convergence);
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

// ──────────────────────────────────────────────────────────────────────────
// Init
// ──────────────────────────────────────────────────────────────────────────
bool NvapiStereoPresenter::Init(uint32_t width, uint32_t height,
                                 uint32_t fseRate,
                                 float separation, float convergence,
                                 bool swapEyes,
                                 const std::string& gameIniPath)
{
    m_width       = width;
    m_height      = height;
    m_fseRate     = fseRate;
    m_separation  = separation;
    m_convergence = convergence;
    m_swapEyes    = swapEyes;
    m_gameIniPath = gameIniPath;

    NvAPI_Status nvs = NvAPI_Initialize();
    if (nvs != NVAPI_OK) {
        NvAPI_ShortString err; NvAPI_GetErrorMessage(nvs, err);
        LOG_ERROR("NvAPI_Initialize: %s", err); return false;
    }
    nvs = NvAPI_Stereo_Enable();
    if (nvs != NVAPI_OK)
        LOG_ERROR("NvAPI_Stereo_Enable failed (%d) — ensure 3D Vision is on in NVIDIA Control Panel", nvs);

    m_msgStop  = false;
    m_initDone = false;
    m_initOk   = false;
    m_msgThread = std::thread([this]() { MsgThreadProc(); });

    // 500 × 10 ms = 5 s.  m_initDone is now set BEFORE RestoreFocusToGame
    // so this timeout is only hit if D3D9 device creation itself hangs.
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

    SetSeparation(separation);
    SetConvergence(convergence);

    m_initialised = true;
    LOG_INFO("NvapiStereoPresenter ready (%ux%u FSE @%uHz, sep=%.1f%%, conv=%.2f)",
             width, height, fseRate, separation, convergence);
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// PresentStereoFrame — SetActiveEye path
//
// SetActiveEye(LEFT)  → StretchRect (left source  → mono backbuffer)
//                        The driver routes this write to the left stereo plane.
// SetActiveEye(RIGHT) → StretchRect (right source → mono backbuffer)
//                        Routed to right stereo plane.
// SetActiveEye(MONO)  → PresentEx
//
// Works on 3DFM-patched drivers because SetActiveEye goes through nvapi.dll.
// If eyes appear swapped, set SwapEyes=true in xr3dv.ini.
// ──────────────────────────────────────────────────────────────────────────
bool NvapiStereoPresenter::PresentStereoFrame(
    ID3D11ShaderResourceView* leftSRV,
    ID3D11ShaderResourceView* rightSRV,
    ID3D11Device*             d3d11Dev)
{
    if (!m_initialised) return false;
    LOG_TRACE("PresentStereoFrame: enter");

    // Honour SwapEyes flag
    ID3D11ShaderResourceView* eyeL = m_swapEyes ? rightSRV : leftSRV;
    ID3D11ShaderResourceView* eyeR = m_swapEyes ? leftSRV  : rightSRV;

    // CPU readback: D3D11 → SYSMEM → DEFAULT for each eye
    if (!BlitD3D11ToSurface(eyeL, d3d11Dev, m_leftSurface.Get(),
                             m_stagingLeft,  m_sysMemLeft))  return false;
    if (!BlitD3D11ToSurface(eyeR, d3d11Dev, m_rightSurface.Get(),
                             m_stagingRight, m_sysMemRight)) return false;

    // Left eye → left stereo plane
    if (m_stereoHandle)
        NvAPI_Stereo_SetActiveEye(m_stereoHandle, NVAPI_STEREO_EYE_LEFT);
    m_device->StretchRect(m_leftSurface.Get(),  nullptr,
                          m_backBuffer.Get(),    nullptr, D3DTEXF_LINEAR);

    // Right eye → right stereo plane
    if (m_stereoHandle)
        NvAPI_Stereo_SetActiveEye(m_stereoHandle, NVAPI_STEREO_EYE_RIGHT);
    m_device->StretchRect(m_rightSurface.Get(), nullptr,
                          m_backBuffer.Get(),    nullptr, D3DTEXF_LINEAR);

    // Reset to mono before present
    if (m_stereoHandle)
        NvAPI_Stereo_SetActiveEye(m_stereoHandle, NVAPI_STEREO_EYE_MONO);

    LOG_TRACE("PresentStereoFrame: PresentEx...");
    HRESULT h = m_device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    if (FAILED(h)) { LOG_ERROR("PresentEx failed: 0x%08X", h); return false; }
    LOG_TRACE("PresentStereoFrame: done");
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// BlitD3D11ToSurface — CPU readback: D3D11 staging → D3D9 SYSMEM → DEFAULT
// ──────────────────────────────────────────────────────────────────────────
bool NvapiStereoPresenter::BlitD3D11ToSurface(
    ID3D11ShaderResourceView*               srv,
    ID3D11Device*                           d3d11Dev,
    IDirect3DSurface9*                      dst,
    ComPtr<ID3D11Texture2D>&                stagingTex,
    ComPtr<IDirect3DSurface9>&              sysMemSurf)
{
    ComPtr<ID3D11Resource> res;
    srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> srcTex;
    if (FAILED(res.As(&srcTex))) {
        LOG_ERROR("BlitD3D11ToSurface: SRV is not Texture2D"); return false;
    }
    D3D11_TEXTURE2D_DESC srcDesc{};
    srcTex->GetDesc(&srcDesc);

    // (Re)create staging texture if size/format changed
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
        st.Width = srcDesc.Width; st.Height = srcDesc.Height;
        st.MipLevels = 1; st.ArraySize = 1;
        st.Format    = srcDesc.Format; st.SampleDesc = {1, 0};
        st.Usage          = D3D11_USAGE_STAGING;
        st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        if (FAILED(d3d11Dev->CreateTexture2D(&st, nullptr, &stagingTex))) {
            LOG_ERROR("CreateTexture2D (staging) failed"); return false;
        }

        HRESULT h = m_device->CreateOffscreenPlainSurface(
            m_stagingWidth, m_stagingHeight, D3DFMT_X8R8G8B8,
            D3DPOOL_SYSTEMMEM, &sysMemSurf, nullptr);
        if (FAILED(h)) {
            LOG_ERROR("CreateOffscreenPlainSurface (SYSMEM) failed: 0x%08X", h);
            return false;
        }
        LOG_VERBOSE("Staging (re)created: %ux%u fmt=%u",
                    m_stagingWidth, m_stagingHeight, (unsigned)m_stagingFormat);
    }

    // Copy D3D11 → staging → map
    ComPtr<ID3D11DeviceContext> ctx;
    d3d11Dev->GetImmediateContext(&ctx);
    ctx->CopyResource(stagingTex.Get(), srcTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) { LOG_ERROR("D3D11 Map failed: 0x%08X", hr); return false; }

    // Copy staging → SYSMEM surface
    D3DLOCKED_RECT lr{};
    hr = sysMemSurf->LockRect(&lr, nullptr, D3DLOCK_DISCARD);
    if (FAILED(hr)) {
        ctx->Unmap(stagingTex.Get(), 0);
        LOG_ERROR("LockRect (SYSMEM) failed: 0x%08X", hr); return false;
    }

    const uint8_t* src  = static_cast<const uint8_t*>(mapped.pData);
    uint8_t*       dstP = static_cast<uint8_t*>(lr.pBits);
    const size_t   rowB = (size_t)srcDesc.Width * 4;
    for (uint32_t row = 0; row < srcDesc.Height; ++row)
        memcpy(dstP + (size_t)row * lr.Pitch,
               src  + (size_t)row * mapped.RowPitch, rowB);

    sysMemSurf->UnlockRect();
    ctx->Unmap(stagingTex.Get(), 0);

    // Upload SYSMEM → DEFAULT
    hr = m_device->UpdateSurface(sysMemSurf.Get(), nullptr, dst, nullptr);
    if (FAILED(hr)) { LOG_ERROR("UpdateSurface failed: 0x%08X", hr); return false; }
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// Stereo parameter control
// ──────────────────────────────────────────────────────────────────────────
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

// ──────────────────────────────────────────────────────────────────────────
// Destructor
// ──────────────────────────────────────────────────────────────────────────
NvapiStereoPresenter::~NvapiStereoPresenter()
{
    m_backBuffer.Reset();
    m_leftSurface.Reset();
    m_rightSurface.Reset();
    m_stagingLeft.Reset();
    m_stagingRight.Reset();
    m_sysMemLeft.Reset();
    m_sysMemRight.Reset();

    if (m_stereoHandle) {
        NvAPI_Stereo_DestroyHandle(m_stereoHandle);
        m_stereoHandle = nullptr;
    }
    m_device.Reset();
    m_d3d9.Reset();

    RemoveGameSubclass();

    m_msgStop = true;
    if (m_hwnd) PostMessageW(m_hwnd, WM_QUIT, 0, 0);
    if (m_msgThread.joinable()) m_msgThread.join();
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }

    NvAPI_Unload();
}

// ──────────────────────────────────────────────────────────────────────────
bool NvapiIsAvailable() {
    if (NvAPI_Initialize() != NVAPI_OK) return false;
    NvU8 en = 0;
    NvAPI_Stereo_IsEnabled(&en);
    NvAPI_Unload();
    return en != 0;
}

} // namespace xr3dv
