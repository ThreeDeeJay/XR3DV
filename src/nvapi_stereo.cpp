//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  Stereo presentation via the NV Packed-Stereo Surface technique:
//    - D3D9Ex device is WINDOWED (borderless topmost) — no FSE, no focus steal.
//    - Each frame a W x (H*2+1) SYSMEM surface is written: left eye in the top
//      half, right eye in the bottom half, NVSTEREOIMAGEHEADER in the last row.
//    - UpdateSurface uploads it to a DEFAULT pool surface, then StretchRect from
//      W x (H*2+1) down to the W x H backbuffer.  The NVIDIA DDI hook intercepts
//      this specific StretchRect and routes it to the hardware stereo planes.
//    - NvAPI_Stereo_Activate is NOT required; packed-surface detection works as
//      long as "Stereo – Enable" is on in NVIDIA Control Panel.
//    - NvAPI_Stereo_SetSeparation / SetConvergence still work via the handle and
//      affect the same OSD values that the 3DV hardware buttons change.

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
// Helpers
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
    if (area > ctx->bestArea) { ctx->bestArea = area; ctx->result = hw; }
    return TRUE;
}

static HWND FindGameWindow(HWND excludeHwnd) {
    FindGameWndCtx ctx{ GetCurrentProcessId(), nullptr, excludeHwnd, 0 };
    EnumWindows(FindGameWndProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

// ---------------------------------------------------------------------------
// Window procedure — WS_EX_TRANSPARENT already passes all input through,
// but we also return MA_NOACTIVATE for extra safety on activate messages.
// ---------------------------------------------------------------------------
static LRESULT CALLBACK XR3DVWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_ACTIVATE)      return 0;      // never let ourselves become active
    return DefWindowProcW(hw, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// MsgThreadProc — creates window, D3D9 windowed device, NVAPI handle,
// packed stereo surfaces, then runs the timer + message pump.
// ---------------------------------------------------------------------------
void NvapiStereoPresenter::MsgThreadProc() {

    // ---- Register window class ----
    static const wchar_t kClass[] = L"XR3DV_Stereo";
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = XR3DVWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);   // idempotent on repeated Init()

    // ---- Create a topmost, non-activating, input-transparent overlay ----
    // WS_EX_NOACTIVATE  — never steals keyboard focus or activation
    // WS_EX_TRANSPARENT — mouse events fall through to the game window below
    // WS_EX_TOPMOST     — stays on top of the game window
    m_hwnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        kClass, L"XR3DV", WS_POPUP,
        0, 0, (int)m_width, (int)m_height,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!m_hwnd) {
        LOG_ERROR("CreateWindowEx failed (GLE=%u)", GetLastError());
        m_initOk.store(false); m_initDone.store(true); return;
    }

    // Show without activating — game keeps focus and audio
    ShowWindow(m_hwnd, SW_SHOWNA);

    // ---- Create D3D9Ex ----
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &m_d3d9);
    if (FAILED(hr)) {
        LOG_ERROR("Direct3DCreate9Ex failed: 0x%08X", hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }

    // ---- Windowed present parameters ----
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth        = m_width;
    pp.BackBufferHeight       = m_height;
    pp.BackBufferFormat       = D3DFMT_X8R8G8B8;
    pp.BackBufferCount        = 1;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow          = m_hwnd;
    pp.Windowed               = TRUE;             // windowed — no FSE, no focus steal
    pp.EnableAutoDepthStencil = FALSE;
    pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;

    hr = m_d3d9->CreateDeviceEx(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, m_hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
        &pp, nullptr,   // nullptr = windowed (no D3DDISPLAYMODEEX needed)
        &m_device);

    if (FAILED(hr)) {
        LOG_ERROR("CreateDeviceEx (windowed %ux%u) failed: 0x%08X", m_width, m_height, hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }
    LOG_INFO("D3D9 windowed device: %ux%u (monitor %uHz)", m_width, m_height, m_fseRate);

    // ---- NVAPI stereo handle ----
    // NvAPI_Stereo_Activate may return "not activated" for windowed devices — that's OK.
    // The packed-surface StretchRect technique works regardless.
    // The handle IS still valid for SetSeparation / SetConvergence / GetSeparation.
    NvAPI_Status nvs = NvAPI_Stereo_CreateHandleFromIUnknown(m_device.Get(), &m_stereoHandle);
    if (nvs != NVAPI_OK) {
        LOG_ERROR("NvAPI_Stereo_CreateHandleFromIUnknown failed (%d) -- separation/convergence hotkeys disabled", nvs);
        // Non-fatal: packed stereo still works; we just can't adjust sep/conv via NVAPI.
    } else {
        NvAPI_Stereo_Activate(m_stereoHandle);   // attempt; OK if it fails for windowed
        NvU8 active = 0;
        NvAPI_Stereo_IsActivated(m_stereoHandle, &active);
        LOG_INFO("NVAPI stereo handle created; IsActivated=%s (not required for packed-surface mode)",
                 active ? "YES" : "NO");
    }

    // ---- Packed stereo surfaces: W x (H*2+1) ----
    // Layout: rows [0, H)   = left eye
    //         rows [H, 2H)  = right eye
    //         row  [2H]     = NvStereoImageHeader (20 bytes) + padding
    const UINT ph = m_height * 2 + 1;

    hr = m_device->CreateOffscreenPlainSurface(
        m_width, ph, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &m_packedSysMem, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("CreateOffscreenPlainSurface (packed SYSMEM %ux%u) failed: 0x%08X",
                  m_width, ph, hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }

    hr = m_device->CreateOffscreenPlainSurface(
        m_width, ph, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &m_packedDefault, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("CreateOffscreenPlainSurface (packed DEFAULT %ux%u) failed: 0x%08X",
                  m_width, ph, hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }

    // ---- Normal W x H mono backbuffer ----
    hr = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_backBuffer);
    if (FAILED(hr)) {
        LOG_ERROR("GetBackBuffer failed: 0x%08X", hr);
        m_initOk.store(false); m_initDone.store(true); return;
    }

    LOG_INFO("Packed stereo surfaces created: %ux%u (each eye %ux%u)",
             m_width, ph, m_width, m_height);

    // ---- Find game window (for WM_ACTIVATE keep-alive) ----
    m_gameHwnd = FindGameWindow(m_hwnd);
    if (m_gameHwnd) {
        LOG_INFO("Game window: %p", (void*)m_gameHwnd);
        // One-shot re-activate to ensure game audio is running at start
        PostMessageW(m_gameHwnd, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), (LPARAM)m_hwnd);
    }

    // ---- Start timers ----
    SetTimer(nullptr, TIMER_INPUT_POLL,  50,  nullptr);  // hotkey-hold polling
    SetTimer(nullptr, TIMER_STEREO_SYNC, 500, nullptr);  // 3DV OSD sync

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
                        // Poll hotkeys every 50 ms — holding a key adjusts continuously at 20 steps/s
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
                    else if (msg.wParam == TIMER_STEREO_SYNC) {
                        // Bidirectional sync with the native 3DV OSD.
                        // If the user tweaked sep/conv via the 3DV hardware buttons,
                        // our values will follow. This also makes the 3DV OSD display
                        // accurate values when XR3DV hotkeys are used.
                        if (m_stereoHandle) {
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

    // NvAPI global init — must precede CreateDevice
    NvAPI_Status nvs = NvAPI_Initialize();
    if (nvs != NVAPI_OK) {
        NvAPI_ShortString err; NvAPI_GetErrorMessage(nvs, err);
        LOG_ERROR("NvAPI_Initialize: %s", err);
        return false;
    }
    nvs = NvAPI_Stereo_Enable();
    if (nvs != NVAPI_OK) {
        LOG_ERROR("NvAPI_Stereo_Enable failed (%d) -- ensure 3D Vision is enabled in NVIDIA Control Panel", nvs);
        return false;
    }

    // Spin up the message/D3D9 thread
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

    // Push initial stereo parameters into the driver
    SetSeparation(separation);
    SetConvergence(convergence);

    m_initialised = true;
    LOG_INFO("NvapiStereoPresenter ready (%ux%u windowed, monitor %uHz, sep=%.1f%%, conv=%.2f)",
             width, height, fseRate, separation, convergence);
    return true;
}

// ---------------------------------------------------------------------------
// PresentStereoFrame
//
// Packs left and right D3D11 eye textures into a W x (H*2+1) surface with the
// NVSTEREOIMAGEHEADER in the last row, then StretchRects it to the backbuffer.
// The NVIDIA DDI hook detects the magic signature and presents in stereo.
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::PresentStereoFrame(
    ID3D11ShaderResourceView* leftSRV,
    ID3D11ShaderResourceView* rightSRV,
    ID3D11Device*             d3d11Dev)
{
    if (!m_initialised) return false;
    LOG_TRACE("PresentStereoFrame: enter");

    // Blit each eye into its half of the packed SYSMEM surface
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
    HRESULT h = m_device->UpdateSurface(
        m_packedSysMem.Get(), nullptr, m_packedDefault.Get(), nullptr);
    if (FAILED(h)) { LOG_ERROR("UpdateSurface failed: 0x%08X", h); return false; }

    // StretchRect from packed W x (H*2+1) → mono backbuffer W x H
    // The NVIDIA DDI hook intercepts this and routes to stereo hardware.
    RECT srcRect{ 0, 0, (LONG)m_width, (LONG)(m_height * 2 + 1) };
    RECT dstRect{ 0, 0, (LONG)m_width, (LONG)m_height };
    h = m_device->StretchRect(
        m_packedDefault.Get(), &srcRect,
        m_backBuffer.Get(),    &dstRect,
        D3DTEXF_POINT);
    if (FAILED(h)) { LOG_ERROR("StretchRect failed: 0x%08X", h); return false; }

    LOG_TRACE("PresentStereoFrame: PresentEx...");
    h = m_device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    if (FAILED(h)) { LOG_ERROR("PresentEx failed: 0x%08X", h); return false; }

    LOG_TRACE("PresentStereoFrame: done");
    return true;
}

// ---------------------------------------------------------------------------
// BlitD3D11ToPacked
// Reads one D3D11 eye texture via CPU staging and copies it into the
// packed SYSMEM surface at the specified y-offset.
// ---------------------------------------------------------------------------
bool NvapiStereoPresenter::BlitD3D11ToPacked(
    ID3D11ShaderResourceView*               srv,
    ID3D11Device*                           d3d11Dev,
    uint32_t                                yOffset,
    ComPtr<ID3D11Texture2D>&                stagingTex)
{
    // Resolve source texture
    ComPtr<ID3D11Resource> res;
    srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> srcTex;
    if (FAILED(res.As(&srcTex))) {
        LOG_ERROR("BlitD3D11ToPacked: SRV resource is not Texture2D");
        return false;
    }
    D3D11_TEXTURE2D_DESC srcDesc{};
    srcTex->GetDesc(&srcDesc);

    // Create or recreate staging texture on size/format change
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

    // Copy into staging texture then map it
    ComPtr<ID3D11DeviceContext> ctx;
    d3d11Dev->GetImmediateContext(&ctx);
    ctx->CopyResource(stagingTex.Get(), srcTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) { LOG_ERROR("D3D11 Map failed: 0x%08X", hr); return false; }

    // Lock the packed SYSMEM surface and memcpy into the correct y-band
    D3DLOCKED_RECT lr{};
    hr = m_packedSysMem->LockRect(&lr, nullptr, 0);
    if (FAILED(hr)) {
        ctx->Unmap(stagingTex.Get(), 0);
        LOG_ERROR("LockRect (packed SYSMEM) failed: 0x%08X", hr);
        return false;
    }

    const uint8_t* src  = static_cast<const uint8_t*>(mapped.pData);
    uint8_t*       dst  = static_cast<uint8_t*>(lr.pBits) + (size_t)yOffset * lr.Pitch;
    const size_t   rowB = (size_t)srcDesc.Width * 4;   // 4 bytes per pixel (X8R8G8B8)

    for (uint32_t row = 0; row < srcDesc.Height; ++row)
        memcpy(dst + (size_t)row * lr.Pitch,
               src + (size_t)row * mapped.RowPitch,
               rowB);

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
