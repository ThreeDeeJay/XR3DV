//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  Stereo presentation: FULLSCREEN-EXCLUSIVE D3D9Ex + NvAPI SetActiveEye.
//
//  Key invariant: D3D9 FSE device ALWAYS uses our own popup window (m_ownedHwnd),
//  never the game's HWND. This is critical to prevent deadlock: CreateDeviceEx
//  internally sends synchronous cross-thread messages to the device window's
//  thread; if that thread is the game thread blocked in xrCreateSession waiting
//  for m_initDone, it cannot pump messages and we deadlock. Our popup's WndProc
//  runs on the msg thread, so no deadlock is possible.
//
//  Audio fix: game window subclass swallows WM_ACTIVATE(INACTIVE),
//  WM_ACTIVATEAPP(FALSE), WM_KILLFOCUS so the game never learns it "lost" focus.
//
//  Mouse-look: Raw Input (WM_INPUT) on the popup delivers relative mouse deltas
//  without cursor interference. Deltas are accumulated in m_mouseDeltaX/Y and
//  consumed each frame by Session::LocateViews to build the head pose. Middle
//  mouse button sets m_recenterRequested.
//
//  Presentation paths (m_stereoActivated):
//    PATH A — SetActiveEye (YES): routes via patched nvapi.dll (3DFM).
//    PATH B — Packed surface (NO): routes via nvlddmkm.sys DDI hook (retail).

#include "pch.h"
#include "nvapi_stereo.h"
#include "config.h"
#include "logging.h"
#include <windowsx.h>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace xr3dv {

static constexpr UINT WM_XR3DV_RETRY_ACTIVATE = WM_APP + 1;

// ---------------------------------------------------------------------------
// Game-window subclass: swallows focus/activate messages so the game engine
// never sees the FSE window steal its activation.
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
// Game window search — EnumWindows only. We deliberately do NOT use
// GetForegroundWindow() here because: (a) it might return a window from
// another process, and (b) our game window might not be foreground yet
// during startup. We want the largest visible top-level window in our process.
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
    FindGameWndCtx ctx{ GetCurrentProcessId(), nullptr, excludeHwnd, 0 };
    EnumWindows(FindGameWndEnum, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

// ---------------------------------------------------------------------------
// Popup window procedure
// Handles Raw Input (WM_INPUT) for mouse-look and hotkeys.
// Input forwarding to game window is intentionally NOT done here —
// games use Raw Input / DirectInput directly; WM_KEY* forwarding is redundant.
// ---------------------------------------------------------------------------
static NvapiStereoPresenter* g_presenterForInput = nullptr;

static LRESULT CALLBACK PopupWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INPUT: {
        if (!g_presenterForInput) break;
        UINT sz = 0;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lp),
                        RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER));
        if (sz == 0 || sz > 256) break;
        BYTE buf[256];
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp),
                            RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) != sz)
            break;
        const auto* ri = reinterpret_cast<const RAWINPUT*>(buf);
        if (ri->header.dwType == RIM_TYPEMOUSE &&
            (ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
            // Accumulate relative mouse deltas (pixel units from driver)
            g_presenterForInput->m_mouseDeltaX.fetch_add(
                static_cast<int32_t>(ri->data.mouse.lLastX),
                std::memory_order_relaxed);
            g_presenterForInput->m_mouseDeltaY.fetch_add(
                static_cast<int32_t>(ri->data.mouse.lLastY),
                std::memory_order_relaxed);
            // Middle button → recenter
            if (ri->data.mouse.usButtonFlags &
                    (RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_BUTTON_3_DOWN))
                g_presenterForInput->m_recenterRequested.store(
                    true, std::memory_order_relaxed);
        }
        break;
    }
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// MsgThreadProc
// ---------------------------------------------------------------------------
void NvapiStereoPresenter::MsgThreadProc()
{
    m_msgThreadId = GetCurrentThreadId();

    // ---- 1. Find game window for subclassing (audio fix) ----
    // We look for the largest visible in-process window, not GetForegroundWindow,
    // because we must NOT send cross-thread messages to the game window here —
    // the game thread is blocked in xrCreateSession waiting for m_initDone.
    // SetWindowLongPtrW(GWLP_WNDPROC) modifies window data without sending
    // messages, so it is safe to call regardless of game thread state.
    m_gameHwnd = FindGameWindow(nullptr);
    if (m_gameHwnd) {
        InstallGameSubclass(m_gameHwnd);
        LOG_INFO("Pre-FSE game window: %p (subclassed=YES)", (void*)m_gameHwnd);
    } else {
        LOG_INFO("Pre-FSE game window: not found yet (no in-process visible window)");
    }

    // ---- 2. Create OUR OWN popup window for D3D9 ----
    // We NEVER use the game HWND for D3D9 FSE. CreateDeviceEx sends internal
    // SendMessage calls to the device window's thread. The game thread is
    // currently blocked in Sleep() in Init(), so it cannot pump messages.
    // Using our own popup (pumped by this thread) prevents that deadlock.
    static const wchar_t kClass[] = L"XR3DV_Popup";
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc); wc.lpfnWndProc = PopupWndProc;
        wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
    }
    g_presenterForInput = this;
    m_ownedHwnd = CreateWindowExW(0, kClass, L"XR3DV", WS_POPUP | WS_VISIBLE,
                                   0, 0, (int)m_width, (int)m_height,
                                   nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_ownedHwnd) {
        LOG_ERROR("CreateWindowEx failed GLE=%u", GetLastError());
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }
    m_hwnd = m_ownedHwnd;
    m_centerX = (int32_t)m_width  / 2;
    m_centerY = (int32_t)m_height / 2;

    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, (int)m_width, (int)m_height,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    BringWindowToTop(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);

    // Register for Raw Input mouse so we get WM_INPUT regardless of focus
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
    rid.usUsage     = 0x02; // HID_USAGE_GENERIC_MOUSE
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = m_hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
        LOG_ERROR("RegisterRawInputDevices failed GLE=%u — mouse-look unavailable",
                  GetLastError());

    // Hide cursor while XR3DV window is active; game cursor still shows when
    // game window has foreground (which it will after subclass + no fights)
    ShowCursor(FALSE);

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

    // ---- 5. NVAPI stereo ----
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

    // Side-by-side packed stereo surface: 2W × (H+1).
    // The extra row at index H holds the NVSTEREOIMAGEHEADER.
    // The NVIDIA DDI hook reads the NV3D signature and routes the
    // left half (x=[0,W)) to the left stereo plane and the right
    // half (x=[W,2W)) to the right stereo plane.
    const UINT pw = m_width * 2;          // surface width = 2x eye width
    const UINT ph = m_height + 1;         // surface height = eye height + header row
    hr  = m_device->CreateOffscreenPlainSurface(pw, ph, D3DFMT_X8R8G8B8,
                                                 D3DPOOL_SYSTEMMEM, &m_packedSysMem,  nullptr);
    hr |= m_device->CreateOffscreenPlainSurface(pw, ph, D3DFMT_X8R8G8B8,
                                                 D3DPOOL_DEFAULT,   &m_packedDefault, nullptr);
    if (FAILED(hr))
        LOG_ERROR("CreateOffscreenPlainSurface (packed %ux%u) failed: 0x%08X", pw, ph, hr);

    hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_backBuffer);
    if (FAILED(hr)) {
        LOG_ERROR("GetBackBuffer failed: 0x%08X", hr);
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }
    LOG_INFO("Stereo surfaces ready: side-by-side %ux%u (each eye %ux%u)",
             pw, ph, m_width, m_height);

    // ---- 7. Update game window post-FSE ----
    {
        HWND found = FindGameWindow(m_hwnd);
        if (found && found != m_gameHwnd) {
            LOG_INFO("Game window updated: %p -> %p", (void*)m_gameHwnd, (void*)found);
            RemoveGameSubclass();
            m_gameHwnd = found;
            InstallGameSubclass(found);
        } else if (!m_gameHwnd && found) {
            m_gameHwnd = found;
            InstallGameSubclass(found);
        }
    }
    if (m_gameHwnd) LOG_INFO("Game window: %p", (void*)m_gameHwnd);

    // ---- 8. Timers (thread-queue) ----
    SetTimer(nullptr, TIMER_INPUT_POLL,  50,  nullptr);
    SetTimer(nullptr, TIMER_STEREO_SYNC, 500, nullptr);

    // ---- 9. Signal init complete ----
    m_initOk.store(true);
    m_initDone.store(true);

    PostThreadMessageW(m_msgThreadId, WM_XR3DV_RETRY_ACTIVATE, 0, 0);

    // ---- Message pump ----
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
                            LOG_INFO("Stereo activated (post dummy Present): SetActiveEye path");
                        } else {
                            LOG_INFO("Stereo activation retry: NO — packed-surface fallback");
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
                                LOG_INFO("Stereo activated (deferred): SetActiveEye path");
                            }
                        }
                        if (active) {
                            float ds = 0.f, dc = 0.f;
                            NvAPI_Stereo_GetSeparation(m_stereoHandle, &ds);
                            NvAPI_Stereo_GetConvergence(m_stereoHandle, &dc);
                            if (fabsf(ds - m_separation)  > 0.05f) {
                                m_separation = ds;
                                LOG_INFO("Separation synced: %.1f%%", m_separation);
                            }
                            if (fabsf(dc - m_convergence) > 0.005f) {
                                m_convergence = dc;
                                LOG_INFO("Convergence synced: %.3f", m_convergence);
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
    ShowCursor(TRUE);
}

// ---------------------------------------------------------------------------
// Init — pumps calling-thread messages while waiting so any synchronous
// cross-thread window messages the OS delivers don't deadlock.
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

    // Wait up to 10 s, pumping messages on the calling thread so any
    // synchronous OS window messages do not deadlock us.
    for (int i = 0; i < 1000 && !m_initDone.load(); ++i) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
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
    if (!m_packedSysMem || !m_packedDefault) {
        LOG_ERROR("Packed surfaces not available"); return false;
    }

    // Side-by-side packed stereo presentation (sView / NVIDIA reference technique).
    //
    // Surface layout: 2W × (H+1)
    //   rows [0, H): left eye in columns [0,W), right eye in columns [W,2W)
    //   row H:       NVSTEREOIMAGEHEADER  (signature 0x4433564e = "NV3D")
    //
    // StretchRect 2W×H → W×H backbuffer.  The NVIDIA DDI hook in nvlddmkm.sys
    // reads the NV3D signature and routes each half to the correct stereo plane
    // at full W×H resolution — no TAB scaling involved.
    //
    // SwapEyes=true in xr3dv.ini uses the SIH_SWAP_EYES header flag to swap
    // which half the driver assigns to each eye.

    ID3D11ShaderResourceView* eyeL = m_swapEyes ? rightSRV : leftSRV;
    ID3D11ShaderResourceView* eyeR = m_swapEyes ? leftSRV  : rightSRV;

    // --- Stage both eyes from D3D11 ---
    auto stageSRV = [&](ID3D11ShaderResourceView* srv,
                        ComPtr<ID3D11Texture2D>&  staging) -> bool {
        ComPtr<ID3D11Resource> res; srv->GetResource(&res);
        ComPtr<ID3D11Texture2D> src;
        if (FAILED(res.As(&src))) { LOG_ERROR("SRV not Texture2D"); return false; }
        D3D11_TEXTURE2D_DESC sd{}; src->GetDesc(&sd);
        if (!staging || m_stagingWidth != sd.Width ||
            m_stagingHeight != sd.Height || m_stagingFormat != sd.Format) {
            staging.Reset();
            m_stagingWidth = sd.Width; m_stagingHeight = sd.Height;
            m_stagingFormat = sd.Format;
            D3D11_TEXTURE2D_DESC st{};
            st.Width = sd.Width; st.Height = sd.Height;
            st.MipLevels = 1; st.ArraySize = 1; st.Format = sd.Format;
            st.SampleDesc = {1,0}; st.Usage = D3D11_USAGE_STAGING;
            st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(d3d11Dev->CreateTexture2D(&st, nullptr, &staging))) {
                LOG_ERROR("CreateTexture2D (staging) failed"); return false;
            }
            LOG_VERBOSE("Staging texture (re)created: %ux%u fmt=%u",
                        sd.Width, sd.Height, (unsigned)sd.Format);
        }
        ComPtr<ID3D11DeviceContext> ctx; d3d11Dev->GetImmediateContext(&ctx);
        ctx->CopyResource(staging.Get(), src.Get());
        return true;
    };
    if (!stageSRV(eyeL, m_stagingLeft))  return false;
    if (!stageSRV(eyeR, m_stagingRight)) return false;

    // --- Lock stereo SYSMEM surface and write both halves ---
    D3DLOCKED_RECT lr{};
    HRESULT h = m_packedSysMem->LockRect(&lr, nullptr, 0);
    if (FAILED(h)) { LOG_ERROR("LockRect (stereo) 0x%08X", h); return false; }

    ComPtr<ID3D11DeviceContext> ctx; d3d11Dev->GetImmediateContext(&ctx);
    const size_t rowBytes = (size_t)m_stagingWidth * 4; // bytes per eye per row
    const size_t xOffR    = rowBytes;                   // right half byte offset

    // Left eye → left half (x=0)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(m_stagingLeft.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
            uint8_t*       dst = static_cast<uint8_t*>(lr.pBits);
            for (uint32_t row = 0; row < m_stagingHeight; ++row)
                memcpy(dst + (size_t)row * lr.Pitch,
                       src + (size_t)row * mapped.RowPitch, rowBytes);
            ctx->Unmap(m_stagingLeft.Get(), 0);
        } else { m_packedSysMem->UnlockRect(); LOG_ERROR("Map (left) failed"); return false; }
    }

    // Right eye → right half (x=W)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(m_stagingRight.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
            uint8_t*       dst = static_cast<uint8_t*>(lr.pBits);
            for (uint32_t row = 0; row < m_stagingHeight; ++row)
                memcpy(dst + (size_t)row * lr.Pitch + xOffR,
                       src + (size_t)row * mapped.RowPitch, rowBytes);
            ctx->Unmap(m_stagingRight.Get(), 0);
        } else { m_packedSysMem->UnlockRect(); LOG_ERROR("Map (right) failed"); return false; }
    }

    // Write NVSTEREOIMAGEHEADER at row H (the extra row beyond the image data)
    {
        auto* hdr = reinterpret_cast<NvStereoImageHeader*>(
            static_cast<uint8_t*>(lr.pBits) + (size_t)m_stagingHeight * lr.Pitch);
        hdr->signature = NVSTEREO_IMAGE_SIGNATURE; // 0x4433564e = "NV3D"
        hdr->width     = m_stagingWidth * 2;       // full side-by-side width
        hdr->height    = m_stagingHeight;
        hdr->bpp       = 32;
        hdr->flags     = 0u; // eye swap handled above via eyeL/eyeR assignment
    }
    m_packedSysMem->UnlockRect();

    // Upload SYSMEM → DEFAULT
    h = m_device->UpdateSurface(m_packedSysMem.Get(), nullptr, m_packedDefault.Get(), nullptr);
    if (FAILED(h)) { LOG_ERROR("UpdateSurface 0x%08X", h); return false; }

    // StretchRect 2W×H → W×H backbuffer (DDI hook intercepts and routes to stereo planes)
    RECT src{ 0, 0, (LONG)(m_stagingWidth * 2), (LONG)m_stagingHeight };
    RECT dst{ 0, 0, (LONG)m_stagingWidth,        (LONG)m_stagingHeight };
    h = m_device->StretchRect(m_packedDefault.Get(), &src,
                               m_backBuffer.Get(),    &dst, D3DTEXF_LINEAR);
    if (FAILED(h)) { LOG_ERROR("StretchRect (stereo) 0x%08X", h); return false; }

    h = m_device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    if (FAILED(h)) { LOG_ERROR("PresentEx 0x%08X", h); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// BlitD3D11ToSurface (PATH A)
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::BlitD3D11ToSurface(
    ID3D11ShaderResourceView*   srv,
    ID3D11Device*               d3d11Dev,
    IDirect3DSurface9*          dstDefault,
    ComPtr<ID3D11Texture2D>&    stagingTex,
    ComPtr<IDirect3DSurface9>&  sysMemSurf)
{
    ComPtr<ID3D11Resource> res; srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> srcTex;
    if (FAILED(res.As(&srcTex))) { LOG_ERROR("SRV not Texture2D"); return false; }
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
            LOG_ERROR("CreateTexture2D staging failed"); return false;
        }
        LOG_VERBOSE("Staging texture (re)created: %ux%u fmt=%u",
                    m_stagingWidth, m_stagingHeight, (unsigned)m_stagingFormat);
    }

    ComPtr<ID3D11DeviceContext> ctx; d3d11Dev->GetImmediateContext(&ctx);
    ctx->CopyResource(stagingTex.Get(), srcTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        LOG_ERROR("D3D11 Map failed"); return false;
    }

    D3DLOCKED_RECT lr{};
    if (FAILED(sysMemSurf->LockRect(&lr, nullptr, 0))) {
        ctx->Unmap(stagingTex.Get(), 0);
        LOG_ERROR("LockRect (sysMemSurf) failed"); return false;
    }

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t*       dst = static_cast<uint8_t*>(lr.pBits);
    const size_t   rb  = (size_t)sd.Width * 4;
    for (uint32_t r = 0; r < sd.Height; ++r)
        memcpy(dst + (size_t)r * lr.Pitch, src + (size_t)r * mapped.RowPitch, rb);

    sysMemSurf->UnlockRect();
    ctx->Unmap(stagingTex.Get(), 0);

    HRESULT h = m_device->UpdateSurface(sysMemSurf.Get(), nullptr, dstDefault, nullptr);
    if (FAILED(h)) { LOG_ERROR("UpdateSurface (per-eye) 0x%08X", h); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// BlitD3D11ToPacked (PATH B)
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::BlitD3D11ToPacked(
    ID3D11ShaderResourceView*   srv,
    ID3D11Device*               d3d11Dev,
    uint32_t                    yOffset,
    ComPtr<ID3D11Texture2D>&    stagingTex)
{
    ComPtr<ID3D11Resource> res; srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> srcTex;
    if (FAILED(res.As(&srcTex))) { LOG_ERROR("SRV not Texture2D"); return false; }
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
            LOG_ERROR("CreateTexture2D staging (packed) failed"); return false;
        }
        LOG_VERBOSE("Staging texture (re)created: %ux%u fmt=%u (yOffset=%u)",
                    m_stagingWidth, m_stagingHeight, (unsigned)m_stagingFormat, yOffset);
    }

    ComPtr<ID3D11DeviceContext> ctx; d3d11Dev->GetImmediateContext(&ctx);
    ctx->CopyResource(stagingTex.Get(), srcTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        LOG_ERROR("D3D11 Map (packed) failed"); return false;
    }

    D3DLOCKED_RECT lr{};
    if (FAILED(m_packedSysMem->LockRect(&lr, nullptr, 0))) {
        ctx->Unmap(stagingTex.Get(), 0);
        LOG_ERROR("LockRect (packedSysMem) failed"); return false;
    }

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t*       dst = static_cast<uint8_t*>(lr.pBits) + (size_t)yOffset * lr.Pitch;
    const size_t   rb  = (size_t)sd.Width * 4;
    for (uint32_t r = 0; r < sd.Height; ++r)
        memcpy(dst + (size_t)r * lr.Pitch, src + (size_t)r * mapped.RowPitch, rb);

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
    g_presenterForInput = nullptr;
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
    if (m_msgThreadId) PostThreadMessageW(m_msgThreadId, WM_QUIT, 0, 0);
    if (m_msgThread.joinable()) m_msgThread.join();
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
