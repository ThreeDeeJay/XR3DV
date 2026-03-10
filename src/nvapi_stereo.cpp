//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  Stereo presentation: FULLSCREEN-EXCLUSIVE D3D9Ex on the GAME'S OWN HWND.
//
//  Why the game's HWND, not a separate popup window:
//    Audio engines (FMOD, WASAPI session management) poll GetForegroundWindow()
//    to decide whether the application has focus. A separate XR3DV popup is the
//    foreground window when 3DV is active, so the game believes it lost focus
//    and pauses audio. Using the game's HWND as the D3D9 FSE window means the
//    game window IS the foreground window at all times — focus, audio and input
//    all work without any cross-thread focus manipulation.
//
//    OpenXR apps render via XR swapchain textures, NOT IDXGISwapChain::Present,
//    so there is no D3D11 present on the game window to conflict with D3D9 FSE.
//
//  Presentation paths (chosen at runtime by m_stereoActivated):
//
//    PATH A — SetActiveEye (IsActivated=YES, 3DFM-patched nvapi.dll):
//      SetActiveEye(L) → StretchRect(leftSurf → backBuffer)
//      SetActiveEye(R) → StretchRect(rightSurf → backBuffer)
//      SetActiveEye(MONO) → PresentEx
//
//    PATH B — Packed-surface fallback (IsActivated=NO, retail nvlddmkm.sys):
//      Write L into rows [0,H), R into [H,2H), NVSTEREOIMAGEHEADER into row [2H]
//      of a W×(2H+1) SYSTEMMEM surface. UpdateSurface → DEFAULT. StretchRect to
//      W×H backbuffer. DDI hook in nvlddmkm.sys intercepts and routes to planes.
//
//  Message pump:
//    No separate window. Timers use SetTimer(nullptr, …) which posts WM_TIMER
//    to the thread queue. Custom messages use PostThreadMessageW(m_msgThreadId,…).
//    PeekMessageW(nullptr,…) retrieves from both window and thread queues.

#include "pch.h"
#include "nvapi_stereo.h"
#include "config.h"
#include "logging.h"
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace xr3dv {

static constexpr UINT WM_XR3DV_RETRY_ACTIVATE = WM_APP + 1;

// ---------------------------------------------------------------------------
// Game-window subclass
// Primary purpose when using game HWND for D3D9: swallow WM_DISPLAYCHANGE and
// WM_SIZE that D3D9 FSE mode-switch sends, so the game doesn't try to resize
// its D3D11 resources. Secondary: swallow deactivation messages as fallback.
// ---------------------------------------------------------------------------
static WNDPROC g_origGameWndProc    = nullptr;
static HWND    g_gameHwndSubclassed = nullptr;

static LRESULT CALLBACK GameWndSubclassProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DISPLAYCHANGE:
        LOG_VERBOSE("Subclass: swallowed WM_DISPLAYCHANGE");
        return 0;
    case WM_SIZE:
        // Swallow resize caused by D3D9 FSE mode change.
        // Let through SIZE_MINIMIZED / SIZE_MAXIMIZED so the game can still
        // react to user-initiated window state changes (Alt+Tab etc.).
        if (wp == SIZE_RESTORED) {
            LOG_VERBOSE("Subclass: swallowed WM_SIZE(RESTORED) — D3D9 mode change");
            return 0;
        }
        break;
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
        LOG_ERROR("SetWindowLongPtrW failed GLE=%u", GetLastError());
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
//
// Strategy: prefer GetForegroundWindow() if it belongs to our process — that
// is the window the audio/input systems are bound to. Fall back to EnumWindows
// largest-area search if nothing in-process has focus right now.
// ---------------------------------------------------------------------------
struct FindGameWndCtx { DWORD pid; HWND result; HWND exclude; LONG bestArea; };

static BOOL CALLBACK FindGameWndEnum(HWND hw, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindGameWndCtx*>(lp);
    if (hw == ctx->exclude) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(hw, &pid);
    if (pid != ctx->pid || !IsWindowVisible(hw)) return TRUE;
    RECT r{}; GetWindowRect(hw, &r);
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > ctx->bestArea) { ctx->bestArea = area; ctx->result = hw; }
    return TRUE;
}

static HWND FindGameWindow(HWND excludeHwnd)
{
    DWORD ourPid = GetCurrentProcessId();

    // Prefer the foreground window — it's the one that owns the audio session
    HWND fg = GetForegroundWindow();
    if (fg && fg != excludeHwnd) {
        DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
        if (pid == ourPid && IsWindowVisible(fg)) return fg;
    }

    // Fallback: largest visible top-level window in our process
    FindGameWndCtx ctx{ ourPid, nullptr, excludeHwnd, 0 };
    EnumWindows(FindGameWndEnum, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

// ---------------------------------------------------------------------------
// Popup window (fallback only — used when no game window is found)
// ---------------------------------------------------------------------------
static LRESULT CALLBACK PopupWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(hw, msg, wp, lp);
}

static HWND CreatePopupWindow(uint32_t width, uint32_t height)
{
    static const wchar_t kClass[] = L"XR3DV_Popup";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = PopupWndProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    HWND hw = CreateWindowExW(0, kClass, L"XR3DV", WS_POPUP | WS_VISIBLE,
                               0, 0, (int)width, (int)height,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hw) return nullptr;
    SetWindowPos(hw, HWND_TOPMOST, 0, 0, (int)width, (int)height,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ShowWindow(hw, SW_SHOWNORMAL);
    SetForegroundWindow(hw);
    SetActiveWindow(hw);
    {
        MSG tmp;
        for (int i = 0; i < 5; ++i) {
            Sleep(20);
            while (PeekMessageW(&tmp, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&tmp); DispatchMessageW(&tmp);
            }
        }
    }
    return hw;
}

// ---------------------------------------------------------------------------
// MsgThreadProc
// ---------------------------------------------------------------------------
void NvapiStereoPresenter::MsgThreadProc()
{
    m_msgThreadId = GetCurrentThreadId();

    // ---- 1. Find game window ----
    // GetForegroundWindow() is checked first: it's the window that owns the
    // process's audio session. For ATS this is the main application window
    // created early in startup; it retains focus throughout OpenXR init.
    m_gameHwnd = FindGameWindow(nullptr);
    if (m_gameHwnd) {
        InstallGameSubclass(m_gameHwnd);
        LOG_INFO("Pre-FSE game window: %p (subclassed=YES)", (void*)m_gameHwnd);
    } else {
        LOG_INFO("Pre-FSE game window: not found — will use popup fallback");
    }

    // ---- 2. Choose D3D9 device window ----
    // Primary: game HWND (stays in focus → audio/input work naturally).
    // Fallback: create a minimal popup window.
    if (m_gameHwnd) {
        m_hwnd      = m_gameHwnd;
        m_ownedHwnd = nullptr;  // we did NOT create this window
        // Ensure the game window is foreground for CreateDeviceEx
        if (GetForegroundWindow() != m_hwnd) {
            SetForegroundWindow(m_hwnd);
            Sleep(50);
        }
    } else {
        m_ownedHwnd = CreatePopupWindow(m_width, m_height);
        if (!m_ownedHwnd) {
            LOG_ERROR("CreatePopupWindow failed GLE=%u", GetLastError());
            m_initOk.store(false); m_initDone.store(true); return;
        }
        m_hwnd = m_ownedHwnd;
    }
    LOG_INFO("D3D9 device window HWND=%p foreground=%s",
             (void*)m_hwnd, GetForegroundWindow() == m_hwnd ? "YES" : "NO");

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
        if (active) m_stereoActivated.store(true);
    }

    // ---- 6. Surfaces ----
    // PATH A: per-eye surfaces for SetActiveEye
    hr  = m_device->CreateOffscreenPlainSurface(m_width, m_height, D3DFMT_X8R8G8B8,
                                                 D3DPOOL_DEFAULT,   &m_leftSurface,  nullptr);
    hr |= m_device->CreateOffscreenPlainSurface(m_width, m_height, D3DFMT_X8R8G8B8,
                                                 D3DPOOL_DEFAULT,   &m_rightSurface, nullptr);
    hr |= m_device->CreateOffscreenPlainSurface(m_width, m_height, D3DFMT_X8R8G8B8,
                                                 D3DPOOL_SYSTEMMEM, &m_sysMemLeft,   nullptr);
    hr |= m_device->CreateOffscreenPlainSurface(m_width, m_height, D3DFMT_X8R8G8B8,
                                                 D3DPOOL_SYSTEMMEM, &m_sysMemRight,  nullptr);
    if (FAILED(hr))
        LOG_ERROR("CreateOffscreenPlainSurface (per-eye) failed: 0x%08X", hr);

    // PATH B: packed W×(2H+1) fallback
    const UINT ph = m_height * 2 + 1;
    hr  = m_device->CreateOffscreenPlainSurface(m_width, ph, D3DFMT_X8R8G8B8,
                                                 D3DPOOL_SYSTEMMEM, &m_packedSysMem,  nullptr);
    hr |= m_device->CreateOffscreenPlainSurface(m_width, ph, D3DFMT_X8R8G8B8,
                                                 D3DPOOL_DEFAULT,   &m_packedDefault, nullptr);
    if (FAILED(hr))
        LOG_ERROR("CreateOffscreenPlainSurface (packed) failed: 0x%08X", hr);

    hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_backBuffer);
    if (FAILED(hr)) {
        LOG_ERROR("GetBackBuffer failed: 0x%08X", hr);
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }
    LOG_INFO("Stereo surfaces ready: per-eye %ux%u  packed %ux%u",
             m_width, m_height, m_width, ph);

    // ---- 7. Update game window reference post-FSE (popup fallback path only) ----
    // When using the game's own HWND we're already on the right window.
    // In the popup fallback, check if a better game window appeared during D3D9 init.
    if (!m_gameHwnd) {
        HWND found = FindGameWindow(m_hwnd);
        if (found) {
            m_gameHwnd = found;
            InstallGameSubclass(found);
            LOG_INFO("Game window (post-FSE): %p", (void*)m_gameHwnd);
        }
    }

    // ---- 8. Timers (thread-queue, no window needed) ----
    SetTimer(nullptr, TIMER_INPUT_POLL,  50,  nullptr);
    SetTimer(nullptr, TIMER_STEREO_SYNC, 500, nullptr);

    // ---- 9. Signal init complete ----
    // No cross-thread calls needed here — game HWND is already in focus.
    m_initOk.store(true);
    m_initDone.store(true);

    PostThreadMessageW(m_msgThreadId, WM_XR3DV_RETRY_ACTIVATE, 0, 0);

    // ---- Message pump (thread + window queue) ----
    while (!m_msgStop.load(std::memory_order_relaxed)) {
        if (MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT,
                                         MWMO_INPUTAVAILABLE) != WAIT_TIMEOUT) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { m_msgStop = true; goto done; }

                if (msg.message == WM_XR3DV_RETRY_ACTIVATE) {
                    if (m_device && m_stereoHandle && !m_stereoActivated.load()) {
                        m_device->Clear(0, nullptr, D3DCLEAR_TARGET,
                                        D3DCOLOR_XRGB(0,0,0), 1.f, 0);
                        m_device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
                        Sleep(50);
                        NvAPI_Stereo_Activate(m_stereoHandle);
                        NvU8 active = 0;
                        NvAPI_Stereo_IsActivated(m_stereoHandle, &active);
                        if (active) {
                            m_stereoActivated.store(true);
                            LOG_INFO("Stereo activated (post dummy Present): using SetActiveEye path");
                        } else {
                            LOG_INFO("Stereo activation retry: still NO — using packed-surface fallback");
                        }
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
                        NvU8 active = 0;
                        NvAPI_Stereo_IsActivated(m_stereoHandle, &active);
                        if (!active) {
                            NvAPI_Stereo_Activate(m_stereoHandle);
                            NvAPI_Stereo_IsActivated(m_stereoHandle, &active);
                            if (active) {
                                m_stereoActivated.store(true);
                                LOG_INFO("Stereo activated (deferred): switching to SetActiveEye path");
                            }
                        }
                        if (active) {
                            float ds = 0.f, dc = 0.f;
                            NvAPI_Stereo_GetSeparation(m_stereoHandle, &ds);
                            NvAPI_Stereo_GetConvergence(m_stereoHandle, &dc);
                            if (fabsf(ds - m_separation)  > 0.05f) {
                                m_separation = ds;
                                LOG_INFO("Separation synced from 3DV OSD: %.1f%%", m_separation);
                            }
                            if (fabsf(dc - m_convergence) > 0.005f) {
                                m_convergence = dc;
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
    m_stereoActivated = false; m_msgThreadId = 0;
    m_msgThread = std::thread([this]() { MsgThreadProc(); });

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
    LOG_TRACE("PresentStereoFrame: enter (path=%s)",
              m_stereoActivated.load() ? "SetActiveEye" : "packed");

    if (m_stereoActivated.load()) {
        // ---- PATH A: SetActiveEye ----
        if (!BlitD3D11ToSurface(leftSRV,  d3d11Dev,
                                 m_leftSurface.Get(),  m_stagingLeft,  m_sysMemLeft))  return false;
        if (!BlitD3D11ToSurface(rightSRV, d3d11Dev,
                                 m_rightSurface.Get(), m_stagingRight, m_sysMemRight)) return false;

        HRESULT h;
        NvAPI_Stereo_SetActiveEye(m_stereoHandle,
                                   m_swapEyes ? NVAPI_STEREO_EYE_RIGHT : NVAPI_STEREO_EYE_LEFT);
        h = m_device->StretchRect(m_leftSurface.Get(),  nullptr,
                                   m_backBuffer.Get(),   nullptr, D3DTEXF_NONE);
        if (FAILED(h)) { LOG_ERROR("StretchRect (left) failed: 0x%08X", h); return false; }

        NvAPI_Stereo_SetActiveEye(m_stereoHandle,
                                   m_swapEyes ? NVAPI_STEREO_EYE_LEFT : NVAPI_STEREO_EYE_RIGHT);
        h = m_device->StretchRect(m_rightSurface.Get(), nullptr,
                                   m_backBuffer.Get(),   nullptr, D3DTEXF_NONE);
        if (FAILED(h)) { LOG_ERROR("StretchRect (right) failed: 0x%08X", h); return false; }

        NvAPI_Stereo_SetActiveEye(m_stereoHandle, NVAPI_STEREO_EYE_MONO);

        h = m_device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
        if (FAILED(h)) { LOG_ERROR("PresentEx failed: 0x%08X", h); return false; }

    } else {
        // ---- PATH B: packed-surface fallback ----
        if (!m_packedSysMem || !m_packedDefault) {
            LOG_ERROR("Packed surfaces not available"); return false;
        }
        if (!BlitD3D11ToPacked(leftSRV,  d3d11Dev, 0,        m_stagingLeft))  return false;
        if (!BlitD3D11ToPacked(rightSRV, d3d11Dev, m_height, m_stagingRight)) return false;

        {
            D3DLOCKED_RECT lr{};
            HRESULT h = m_packedSysMem->LockRect(&lr, nullptr, 0);
            if (FAILED(h)) { LOG_ERROR("LockRect (header) failed: 0x%08X", h); return false; }
            auto* hdr = reinterpret_cast<NvStereoImageHeader*>(
                static_cast<uint8_t*>(lr.pBits) + (size_t)2 * m_height * lr.Pitch);
            hdr->signature = NVSTEREO_IMAGE_SIGNATURE;
            hdr->width = m_width; hdr->height = m_height;
            hdr->bpp   = 32;      hdr->flags  = m_swapEyes ? 1u : 0u;
            m_packedSysMem->UnlockRect();
        }

        HRESULT h = m_device->UpdateSurface(
            m_packedSysMem.Get(), nullptr, m_packedDefault.Get(), nullptr);
        if (FAILED(h)) { LOG_ERROR("UpdateSurface failed: 0x%08X", h); return false; }

        RECT src{ 0, 0, (LONG)m_width, (LONG)(m_height * 2 + 1) };
        RECT dst{ 0, 0, (LONG)m_width, (LONG)m_height };
        h = m_device->StretchRect(
            m_packedDefault.Get(), &src, m_backBuffer.Get(), &dst, D3DTEXF_POINT);
        if (FAILED(h)) { LOG_ERROR("StretchRect (packed) failed: 0x%08X", h); return false; }

        h = m_device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
        if (FAILED(h)) { LOG_ERROR("PresentEx failed: 0x%08X", h); return false; }
    }

    LOG_TRACE("PresentStereoFrame: done");
    return true;
}

// ---------------------------------------------------------------------------
// BlitD3D11ToSurface  (PATH A — SetActiveEye)
// D3D11 SRV → D3D9 DEFAULT surface via CPU readback
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::BlitD3D11ToSurface(
    ID3D11ShaderResourceView*               srv,
    ID3D11Device*                           d3d11Dev,
    IDirect3DSurface9*                      dstDefault,
    ComPtr<ID3D11Texture2D>&                stagingTex,
    ComPtr<IDirect3DSurface9>&              sysMemSurf)
{
    ComPtr<ID3D11Resource> res; srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> srcTex;
    if (FAILED(res.As(&srcTex))) {
        LOG_ERROR("BlitD3D11ToSurface: SRV is not Texture2D"); return false;
    }
    D3D11_TEXTURE2D_DESC sd{}; srcTex->GetDesc(&sd);

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
        LOG_VERBOSE("Staging texture (re)created: %ux%u fmt=%u",
                    m_stagingWidth, m_stagingHeight, (unsigned)m_stagingFormat);
    }

    ComPtr<ID3D11DeviceContext> ctx; d3d11Dev->GetImmediateContext(&ctx);
    ctx->CopyResource(stagingTex.Get(), srcTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) { LOG_ERROR("D3D11 Map failed: 0x%08X", hr); return false; }

    D3DLOCKED_RECT lr{};
    hr = sysMemSurf->LockRect(&lr, nullptr, 0);
    if (FAILED(hr)) {
        ctx->Unmap(stagingTex.Get(), 0);
        LOG_ERROR("LockRect (sysMemSurf) failed: 0x%08X", hr); return false;
    }

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t*       dst = static_cast<uint8_t*>(lr.pBits);
    const size_t   rowB = (size_t)sd.Width * 4;
    for (uint32_t row = 0; row < sd.Height; ++row)
        memcpy(dst + (size_t)row * lr.Pitch,
               src + (size_t)row * mapped.RowPitch, rowB);

    sysMemSurf->UnlockRect();
    ctx->Unmap(stagingTex.Get(), 0);

    hr = m_device->UpdateSurface(sysMemSurf.Get(), nullptr, dstDefault, nullptr);
    if (FAILED(hr)) { LOG_ERROR("UpdateSurface (per-eye) failed: 0x%08X", hr); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// BlitD3D11ToPacked  (PATH B — packed-surface fallback)
// D3D11 SRV → packed SYSTEMMEM surface at yOffset
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::BlitD3D11ToPacked(
    ID3D11ShaderResourceView*   srv,
    ID3D11Device*               d3d11Dev,
    uint32_t                    yOffset,
    ComPtr<ID3D11Texture2D>&    stagingTex)
{
    ComPtr<ID3D11Resource> res; srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> srcTex;
    if (FAILED(res.As(&srcTex))) {
        LOG_ERROR("BlitD3D11ToPacked: SRV is not Texture2D"); return false;
    }
    D3D11_TEXTURE2D_DESC sd{}; srcTex->GetDesc(&sd);

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
            LOG_ERROR("CreateTexture2D (staging packed) failed"); return false;
        }
        LOG_VERBOSE("Staging texture (re)created: %ux%u fmt=%u (yOffset=%u)",
                    m_stagingWidth, m_stagingHeight, (unsigned)m_stagingFormat, yOffset);
    }

    ComPtr<ID3D11DeviceContext> ctx; d3d11Dev->GetImmediateContext(&ctx);
    ctx->CopyResource(stagingTex.Get(), srcTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) { LOG_ERROR("D3D11 Map failed: 0x%08X", hr); return false; }

    D3DLOCKED_RECT lr{};
    hr = m_packedSysMem->LockRect(&lr, nullptr, 0);
    if (FAILED(hr)) {
        ctx->Unmap(stagingTex.Get(), 0);
        LOG_ERROR("LockRect (packedSysMem) failed: 0x%08X", hr); return false;
    }

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t*       dst = static_cast<uint8_t*>(lr.pBits) + (size_t)yOffset * lr.Pitch;
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
    m_backBuffer.Reset();
    m_leftSurface.Reset();  m_rightSurface.Reset();
    m_sysMemLeft.Reset();   m_sysMemRight.Reset();
    m_packedSysMem.Reset(); m_packedDefault.Reset();
    m_stagingLeft.Reset();  m_stagingRight.Reset();
    if (m_stereoHandle) {
        NvAPI_Stereo_DestroyHandle(m_stereoHandle);
        m_stereoHandle = nullptr;
    }
    m_device.Reset(); m_d3d9.Reset();
    RemoveGameSubclass();

    m_msgStop = true;
    if (m_msgThreadId)
        PostThreadMessageW(m_msgThreadId, WM_QUIT, 0, 0);
    if (m_msgThread.joinable()) m_msgThread.join();

    // Only destroy window if we created it (popup fallback)
    if (m_ownedHwnd) { DestroyWindow(m_ownedHwnd); m_ownedHwnd = nullptr; }
    m_hwnd = nullptr;

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
