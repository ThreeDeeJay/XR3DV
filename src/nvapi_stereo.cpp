//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  Stereo presentation using the NV Packed-Stereo Surface technique on a
//  FULLSCREEN-EXCLUSIVE D3D9Ex device.
//
//  Why FSE is required:
//    NvAPI_Stereo_Activate (and therefore the DDI-level StretchRect hook that
//    splits packed surfaces) only works on FSE devices on drivers >= 426.06.
//    Windowed devices return IsActivated=NO and the StretchRect just scales the
//    surface normally, producing a squished top-and-bottom image.
//
//  Focus / audio problem with FSE — solved by game-window subclassing:
//    When our FSE device is created, Win32 sends WM_ACTIVATE(WA_INACTIVE),
//    WM_KILLFOCUS, and WM_ACTIVATEAPP(FALSE) to the previously-active game
//    window.  Game engines that hook these messages silence audio and stop
//    processing input.  Because we are a DLL running inside the game process
//    we can subclass the game's WndProc and swallow those deactivation
//    messages *before* they reach the engine.  The game never learns that it
//    lost focus, audio keeps running, and UI menus remain clickable.
//    Mouse / keyboard events arriving at our FSE window are forwarded via
//    PostMessage to the game window so the game still receives input.

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
// Game-window subclass — installed BEFORE FSE device creation so it intercepts
// the deactivation messages that FSE creation triggers.
// ---------------------------------------------------------------------------
static WNDPROC g_origGameWndProc = nullptr;
static HWND    g_gameHwndForSubclass = nullptr;

static LRESULT CALLBACK GameWndSubclassProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    // Swallow "you are being deactivated" messages entirely.
    // We never let these reach the engine so audio and input keep running.
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) return 0;
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
    g_gameHwndForSubclass = gameHwnd;
    g_origGameWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(gameHwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(GameWndSubclassProc)));
    if (g_origGameWndProc)
        LOG_INFO("Game window subclassed (WndProc=%p)", (void*)g_origGameWndProc);
    else
        LOG_ERROR("SetWindowLongPtrW (subclass) failed: GLE=%u", GetLastError());
}

static void RemoveGameSubclass()
{
    if (!g_gameHwndForSubclass || !g_origGameWndProc) return;
    SetWindowLongPtrW(g_gameHwndForSubclass, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(g_origGameWndProc));
    g_origGameWndProc    = nullptr;
    g_gameHwndForSubclass = nullptr;
    LOG_INFO("Game window WndProc restored");
}

// ---------------------------------------------------------------------------
// Find game's main window (largest visible top-level window in our process
// that isn't the XR3DV D3D9 window).
// ---------------------------------------------------------------------------
struct FindGameWndCtx { DWORD pid; HWND result; HWND exclude; LONG bestArea; };

static BOOL CALLBACK FindGameWndProc(HWND hw, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindGameWndCtx*>(lp);
    if (hw == ctx->exclude) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hw, &pid);
    if (pid != ctx->pid) return TRUE;
    if (!IsWindowVisible(hw)) return TRUE;
    RECT r{};
    GetWindowRect(hw, &r);
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > ctx->bestArea) { ctx->bestArea = area; ctx->result = hw; }
    return TRUE;
}

static HWND FindGameWindow(HWND excludeHwnd)
{
    FindGameWndCtx ctx{ GetCurrentProcessId(), nullptr, excludeHwnd, 0 };
    EnumWindows(FindGameWndProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

// ---------------------------------------------------------------------------
// FSE window procedure — forwards all mouse/keyboard input to the game window.
// ---------------------------------------------------------------------------
struct WndProcCtx { HWND gameHwnd; };
static WndProcCtx g_wpCtx{};

static LRESULT CALLBACK XR3DVWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND gh = g_wpCtx.gameHwnd;
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:   case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:   case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:   case WM_MBUTTONDBLCLK:
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
// Restore focus to the game window after FSE device creation.
// Uses AttachThreadInput so SetFocus works across thread boundaries.
// ---------------------------------------------------------------------------
static void RestoreFocusToGame(HWND gameHwnd, HWND ourHwnd)
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
// MsgThreadProc — ALL D3D9 work + message pump
// ---------------------------------------------------------------------------
void NvapiStereoPresenter::MsgThreadProc()
{
    // ---- 1. Find game window and install subclass BEFORE taking FSE ----
    // We look before creating our own window so we get the largest existing
    // visible window (the game's main window).
    m_gameHwnd = GetForegroundWindow();
    {
        DWORD fgPid = 0;
        GetWindowThreadProcessId(m_gameHwnd, &fgPid);
        if (fgPid != GetCurrentProcessId()) m_gameHwnd = nullptr;
    }
    // Install subclass so deactivation messages caused by FSE are swallowed
    if (m_gameHwnd) InstallGameSubclass(m_gameHwnd);

    // ---- 2. Create and activate our FSE window ----
    static const wchar_t kClass[] = L"XR3DV_D3D9";
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = XR3DVWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(0, kClass, L"XR3DV",
                              WS_POPUP, 0, 0, (int)m_width, (int)m_height,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!m_hwnd) {
        LOG_ERROR("CreateWindowEx failed (GLE=%u)", GetLastError());
        m_initOk.store(false); m_initDone.store(true); return;
    }

    g_wpCtx.gameHwnd = m_gameHwnd;

    ShowWindow(m_hwnd, SW_SHOW);
    BringWindowToTop(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);

    // Pump briefly to let window reach foreground
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

    // ---- 3. Create D3D9Ex ----
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &m_d3d9);
    if (FAILED(hr)) {
        LOG_ERROR("Direct3DCreate9Ex failed: 0x%08X", hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }

    // ---- 4. Enumerate / validate adapter mode ----
    {
        UINT cnt = m_d3d9->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
        bool found = false;
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

    // ---- 5. Create FSE device ----
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

    // ---- 6. NVAPI stereo handle + activate ----
    NvAPI_Status nvs = NvAPI_Stereo_CreateHandleFromIUnknown(m_device.Get(), &m_stereoHandle);
    if (nvs != NVAPI_OK) {
        LOG_ERROR("NvAPI_Stereo_CreateHandleFromIUnknown failed (%d)", nvs);
        RemoveGameSubclass();
        m_initOk.store(false); m_initDone.store(true); return;
    }
    nvs = NvAPI_Stereo_Activate(m_stereoHandle);
    if (nvs != NVAPI_OK) {
        LOG_ERROR("NvAPI_Stereo_Activate failed (%d) — ensure 3D Vision is on in NVIDIA Control Panel", nvs);
        // Non-fatal — packed surface may still work if 3DV is enabled globally
    }
    {
        NvU8 active = 0;
        NvAPI_Stereo_IsActivated(m_stereoHandle, &active);
        LOG_INFO("NVAPI stereo activated: %s", active ? "YES" : "NO — check NVIDIA Control Panel");
    }

    // ---- 7. Packed stereo surfaces: W x (H*2+1) ----
    const UINT ph = m_height * 2 + 1;
    hr = m_device->CreateOffscreenPlainSurface(
        m_width, ph, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &m_packedSysMem, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("CreateOffscreenPlainSurface (SYSMEM %ux%u) failed: 0x%08X", m_width, ph, hr);
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }
    hr = m_device->CreateOffscreenPlainSurface(
        m_width, ph, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &m_packedDefault, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("CreateOffscreenPlainSurface (DEFAULT %ux%u) failed: 0x%08X", m_width, ph, hr);
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }

    // ---- 8. Mono backbuffer ----
    hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_backBuffer);
    if (FAILED(hr)) {
        LOG_ERROR("GetBackBuffer failed: 0x%08X", hr);
        RemoveGameSubclass(); m_initOk.store(false); m_initDone.store(true); return;
    }
    LOG_INFO("Packed stereo surfaces: %ux%u (each eye %ux%u)", m_width, ph, m_width, m_height);

    // ---- 9. Find game window (if not found before window creation) ----
    if (!m_gameHwnd) {
        m_gameHwnd = FindGameWindow(m_hwnd);
        if (m_gameHwnd) {
            g_wpCtx.gameHwnd = m_gameHwnd;
            InstallGameSubclass(m_gameHwnd);
        }
    }
    if (m_gameHwnd) {
        LOG_INFO("Game window: %p", (void*)m_gameHwnd);
        // Restore focus to game — our subclass is already in place so the
        // game won't see any deactivation when we take focus back.
        RestoreFocusToGame(m_gameHwnd, m_hwnd);
    }

    // ---- 10. Timers ----
    SetTimer(nullptr, TIMER_INPUT_POLL,  50,  nullptr);
    SetTimer(nullptr, TIMER_STEREO_SYNC, 500, nullptr);

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
                        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                        if (ctrl) {
                            float sep  = m_separation;
                            float conv = m_convergence;
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
                        float drvSep = 0.0f, drvConv = 0.0f;
                        NvAPI_Stereo_GetSeparation(m_stereoHandle, &drvSep);
                        NvAPI_Stereo_GetConvergence(m_stereoHandle, &drvConv);
                        if (fabsf(drvSep  - m_separation)  > 0.05f) {
                            m_separation = drvSep;
                            LOG_INFO("Separation synced from 3DV OSD: %.1f%%", m_separation);
                        }
                        if (fabsf(drvConv - m_convergence) > 0.005f) {
                            m_convergence = drvConv;
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

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// PresentStereoFrame — packed-surface StretchRect technique
//
// Surface layout (W x H*2+1):
//   rows [0,   H)  — left eye
//   rows [H,  2H)  — right eye
//   row  [2H]      — NvStereoImageHeader (20 bytes), rest of row unused
//
// The NVIDIA DDI hook detects the magic signature in that last row and routes
// the StretchRect to the stereo hardware planes.  Requires FSE + 3DV active.
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::PresentStereoFrame(
    ID3D11ShaderResourceView* leftSRV,
    ID3D11ShaderResourceView* rightSRV,
    ID3D11Device*             d3d11Dev)
{
    if (!m_initialised) return false;
    LOG_TRACE("PresentStereoFrame: enter");

    // Copy both eyes into the packed SYSMEM surface
    if (!BlitD3D11ToPacked(leftSRV,  d3d11Dev, 0,        m_stagingLeft))  return false;
    if (!BlitD3D11ToPacked(rightSRV, d3d11Dev, m_height, m_stagingRight)) return false;

    // Write NVSTEREOIMAGEHEADER into the last (+1) row
    {
        D3DLOCKED_RECT lr{};
        HRESULT h = m_packedSysMem->LockRect(&lr, nullptr, 0);
        if (FAILED(h)) { LOG_ERROR("LockRect (header) failed: 0x%08X", h); return false; }

        NvStereoImageHeader* hdr =
            reinterpret_cast<NvStereoImageHeader*>(
                static_cast<uint8_t*>(lr.pBits) + (size_t)2 * m_height * lr.Pitch);
        hdr->signature = NVSTEREO_IMAGE_SIGNATURE;
        hdr->width     = m_width;
        hdr->height    = m_height;
        hdr->bpp       = 32;
        hdr->flags     = m_swapEyes ? 1u : 0u;

        m_packedSysMem->UnlockRect();
    }

    // Upload SYSMEM → DEFAULT
    HRESULT h = m_device->UpdateSurface(m_packedSysMem.Get(), nullptr,
                                         m_packedDefault.Get(), nullptr);
    if (FAILED(h)) { LOG_ERROR("UpdateSurface failed: 0x%08X", h); return false; }

    // StretchRect W×(H*2+1) → W×H backbuffer
    // The NVIDIA DDI hook intercepts this and routes to stereo planes
    RECT srcRect{ 0, 0, (LONG)m_width, (LONG)(m_height * 2 + 1) };
    RECT dstRect{ 0, 0, (LONG)m_width, (LONG)m_height };
    h = m_device->StretchRect(m_packedDefault.Get(), &srcRect,
                               m_backBuffer.Get(),   &dstRect,
                               D3DTEXF_POINT);
    if (FAILED(h)) { LOG_ERROR("StretchRect failed: 0x%08X", h); return false; }

    LOG_TRACE("PresentStereoFrame: PresentEx...");
    h = m_device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    if (FAILED(h)) { LOG_ERROR("PresentEx failed: 0x%08X", h); return false; }

    LOG_TRACE("PresentStereoFrame: done");
    return true;
}

// ---------------------------------------------------------------------------
// BlitD3D11ToPacked — CPU readback into the correct y-band of m_packedSysMem
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::BlitD3D11ToPacked(
    ID3D11ShaderResourceView*               srv,
    ID3D11Device*                           d3d11Dev,
    uint32_t                                yOffset,
    ComPtr<ID3D11Texture2D>&                stagingTex)
{
    ComPtr<ID3D11Resource> res;
    srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> srcTex;
    if (FAILED(res.As(&srcTex))) {
        LOG_ERROR("BlitD3D11ToPacked: SRV resource is not Texture2D");
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
        m_stagingWidth  = srcDesc.Width;
        m_stagingHeight = srcDesc.Height;
        m_stagingFormat = srcDesc.Format;

        D3D11_TEXTURE2D_DESC st{};
        st.Width = srcDesc.Width; st.Height = srcDesc.Height;
        st.MipLevels = 1; st.ArraySize = 1;
        st.Format    = srcDesc.Format;
        st.SampleDesc = {1, 0};
        st.Usage          = D3D11_USAGE_STAGING;
        st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        if (FAILED(d3d11Dev->CreateTexture2D(&st, nullptr, &stagingTex))) {
            LOG_ERROR("Failed to create D3D11 staging texture");
            return false;
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
        LOG_ERROR("LockRect (packed SYSMEM) failed: 0x%08X", hr);
        return false;
    }

    const uint8_t* src  = static_cast<const uint8_t*>(mapped.pData);
    uint8_t*       dst  = static_cast<uint8_t*>(lr.pBits) + (size_t)yOffset * lr.Pitch;
    const size_t   rowB = (size_t)srcDesc.Width * 4;

    for (uint32_t row = 0; row < srcDesc.Height; ++row)
        memcpy(dst + (size_t)row * lr.Pitch,
               src + (size_t)row * mapped.RowPitch, rowB);

    m_packedSysMem->UnlockRect();
    ctx->Unmap(stagingTex.Get(), 0);
    return true;
}

// ---------------------------------------------------------------------------
// Stereo parameter control
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
// Destructor
// ---------------------------------------------------------------------------
NvapiStereoPresenter::~NvapiStereoPresenter() {
    m_backBuffer.Reset();
    m_packedSysMem.Reset();
    m_packedDefault.Reset();
    m_stagingLeft.Reset();
    m_stagingRight.Reset();

    if (m_stereoHandle) {
        NvAPI_Stereo_DestroyHandle(m_stereoHandle);
        m_stereoHandle = nullptr;
    }
    m_device.Reset();
    m_d3d9.Reset();

    // Remove game window subclass before the thread (and its hook) dies
    RemoveGameSubclass();

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
