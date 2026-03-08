//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#include "pch.h"
#include "present_hook.h"
#include "config.h"
#include "logging.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace xr3dv {

// ---------------------------------------------------------------------------
// Fullscreen-triangle blit shaders
// VS generates a screen-filling triangle from SV_VertexID, no vertex buffer.
// PS samples the source texture and writes it to the RTV.
// ---------------------------------------------------------------------------
static const char kBlitVS[] =
    "void main(uint id : SV_VertexID,"
    "          out float4 pos : SV_Position,"
    "          out float2 uv  : TEXCOORD0) {"
    "  uv  = float2((id << 1) & 2, id & 2);"
    "  pos = float4(uv * float2(2,-2) + float2(-1,1), 0, 1);"
    "}";

static const char kBlitPS[] =
    "Texture2D    tex : register(t0);"
    "SamplerState smp : register(s0);"
    "float4 main(float4 pos : SV_Position,"
    "            float2 uv  : TEXCOORD0) : SV_Target {"
    "  return tex.Sample(smp, uv);"
    "}";

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
PresentHook& PresentHook::Get() {
    static PresentHook inst;
    return inst;
}
PresentHook::~PresentHook() { Shutdown(); }

// ---------------------------------------------------------------------------
// VTable patching — page is copy-on-write so write only affects this process
// ---------------------------------------------------------------------------
void PresentHook::PatchVTable(void* obj, size_t idx, void* hook, void** orig) {
    void** vt = *reinterpret_cast<void***>(obj);
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *orig   = vt[idx];
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}

// ---------------------------------------------------------------------------
// Static hook stub and original pointer
// IDXGISwapChain::Present is at vtable slot 8
// ---------------------------------------------------------------------------
static HRESULT (STDMETHODCALLTYPE* g_origPresent)(
    IDXGISwapChain*, UINT, UINT) = nullptr;

static HRESULT STDMETHODCALLTYPE HookPresent(
    IDXGISwapChain* pSC, UINT sync, UINT flags)
{
    // Inject stereo frame into back buffer BEFORE the original Present
    PresentHook::Get().OnPresent(pSC, sync, flags);
    return g_origPresent(pSC, sync, flags);
}

// ---------------------------------------------------------------------------
// HookPresent — create a temporary 1×1 swap chain on the game's D3D11 device
// just to obtain a valid IDXGISwapChain* for vtable patching, then release it.
// The vtable write persists because all IDXGISwapChain objects share the same
// vftable in dxgi.dll (it's a single table in the DLL's data segment).
// ---------------------------------------------------------------------------
bool PresentHook::HookPresent(ID3D11Device* dev) {
    if (m_presentHooked) return true;

    // Get IDXGIFactory from the device's adapter
    ComPtr<IDXGIDevice>  dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory> factory;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev))) ||
        FAILED(dxgiDev->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        LOG_ERROR("HookPresent: failed to get IDXGIFactory from D3D11 device");
        return false;
    }

    // Create a tiny 1x1 windowed swap chain just for vtable access
    static const wchar_t kCls[] = L"XR3DV_VtPatch";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kCls;
    RegisterClassExW(&wc);
    HWND hw = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                               kCls, L"", WS_POPUP, 0, 0, 1, 1,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (!hw) {
        LOG_ERROR("HookPresent: CreateWindowEx failed (GLE=%u)", GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount       = 1;
    scd.BufferDesc.Width  = 1;
    scd.BufferDesc.Height = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = hw;
    scd.SampleDesc        = {1, 0};
    scd.Windowed          = TRUE;

    ComPtr<IDXGISwapChain> dummy;
    HRESULT hr = factory->CreateSwapChain(dev, &scd, &dummy);
    if (FAILED(hr)) {
        LOG_ERROR("HookPresent: dummy CreateSwapChain failed: 0x%08X", hr);
        DestroyWindow(hw);
        return false;
    }

    // Patch slot 8 (Present) in the shared IDXGISwapChain vftable
    PatchVTable(dummy.Get(), 8,
                reinterpret_cast<void*>(&HookPresent),
                reinterpret_cast<void**>(&g_origPresent));
    // dummy is released here — vtable patch persists in dxgi.dll's .data page
    dummy.Reset();
    DestroyWindow(hw);

    m_presentHooked = true;
    LOG_INFO("IDXGISwapChain::Present hooked (slot 8)");
    return true;
}

// ---------------------------------------------------------------------------
// InitStereo — called from Session::InitD3D11
// ---------------------------------------------------------------------------
bool PresentHook::InitStereo(ID3D11Device* dev,
                               float sep, float conv,
                               const std::string& gameIniPath)
{
    // If reinitialising (second session), do a light reset
    if (m_initialised) {
        LOG_INFO("PresentHook: reinitialising for new session");
        // Reuse the existing NVAPI handle if same device, else recreate
        if (m_stereoHandle && m_dev.Get() != dev) {
            NvAPI_Stereo_DestroyHandle(m_stereoHandle);
            m_stereoHandle = nullptr;
        }
        m_dev.Reset(); m_ctx.Reset();
        m_backBufRTV.Reset();
        m_cachedLeftSRV.Reset(); m_cachedRightSRV.Reset();
        m_cachedLeftTex.Reset(); m_cachedRightTex.Reset();
        m_gameSwapChain = nullptr;
        m_scW = m_scH = 0;
        m_initialised = false;
    }

    m_dev.Attach(dev); dev->AddRef();
    dev->GetImmediateContext(&m_ctx);
    m_separation  = sep;
    m_convergence = conv;
    m_gameIniPath = gameIniPath;

    // 1. Hook IDXGISwapChain::Present via vtable (idempotent)
    if (!HookPresent(dev)) return false;

    // 2. NVAPI stereo on the GAME'S D3D11 device — this is where 3DV is active
    if (!m_stereoHandle) {
        NvAPI_Status nvs = NvAPI_Initialize();
        if (nvs != NVAPI_OK) {
            NvAPI_ShortString s; NvAPI_GetErrorMessage(nvs, s);
            LOG_ERROR("NvAPI_Initialize failed: %s", s);
            return false;
        }
        nvs = NvAPI_Stereo_Enable();
        if (nvs != NVAPI_OK)
            LOG_ERROR("NvAPI_Stereo_Enable failed (%d) — ensure 3D Vision is on", nvs);

        nvs = NvAPI_Stereo_CreateHandleFromIUnknown(dev, &m_stereoHandle);
        if (nvs != NVAPI_OK) {
            LOG_ERROR("NvAPI_Stereo_CreateHandleFromIUnknown (D3D11) failed: %d", nvs);
            return false;
        }

        NvU8 active = 0;
        NvAPI_Stereo_IsActivated(m_stereoHandle, &active);
        LOG_INFO("NVAPI stereo on game D3D11 device: %s",
                 active ? "ACTIVATED — 3D Vision engaged" : "NOT ACTIVATED (driver will activate at first frame)");
        // Note: stereo may not be 'activated' yet until the game presents a frame
        // that the driver recognises as needing stereo. NvAPI_Stereo_Activate is not
        // needed here — the driver activates automatically when it detects stereo content.
    }

    // 3. Blit shaders
    if (!EnsureBlitShaders()) return false;

    // 4. Apply initial parameters
    SetSeparation(sep);
    SetConvergence(conv);

    // 5. Start hotkey thread (only once)
    if (!m_hotkeyThread.joinable()) {
        m_hotkeyStop   = false;
        m_hotkeyThread = std::thread([this]() { HotkeyThreadProc(); });
    }

    m_initialised = true;
    LOG_INFO("PresentHook initialised: sep=%.1f%% conv=%.3f", sep, conv);
    return true;
}

// ---------------------------------------------------------------------------
// EnsureBlitShaders
// ---------------------------------------------------------------------------
bool PresentHook::EnsureBlitShaders() {
    if (m_blitVS && m_blitPS && m_sampler) return true;

    auto compile = [](const char* src, const char* tgt, ID3DBlob** out) -> bool {
        ComPtr<ID3DBlob> err;
        HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                                 "main", tgt, 0, 0, out, &err);
        if (FAILED(hr)) {
            LOG_ERROR("Shader compile (%s) failed: %s", tgt,
                      err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!compile(kBlitVS, "vs_5_0", &vsBlob)) return false;
    if (!compile(kBlitPS, "ps_5_0", &psBlob)) return false;

    if (FAILED(m_dev->CreateVertexShader(vsBlob->GetBufferPointer(),
                                          vsBlob->GetBufferSize(), nullptr, &m_blitVS)) ||
        FAILED(m_dev->CreatePixelShader(psBlob->GetBufferPointer(),
                                         psBlob->GetBufferSize(), nullptr, &m_blitPS))) {
        LOG_ERROR("CreateVertexShader/PixelShader failed");
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(m_dev->CreateSamplerState(&sd, &m_sampler))) {
        LOG_ERROR("CreateSamplerState failed");
        return false;
    }

    LOG_INFO("Blit shaders compiled");
    return true;
}

// ---------------------------------------------------------------------------
// SetPendingFrame — store XR eye textures for injection at next Present
// ---------------------------------------------------------------------------
void PresentHook::SetPendingFrame(ID3D11ShaderResourceView* leftSRV,
                                   ID3D11ShaderResourceView* rightSRV)
{
    if (!leftSRV || !rightSRV) return;
    ComPtr<ID3D11Resource> rL, rR;
    leftSRV->GetResource(&rL);
    rightSRV->GetResource(&rR);
    std::lock_guard<std::mutex> lk(m_pendingMtx);
    rL.As(&m_pendingLeft);
    rR.As(&m_pendingRight);
}

// ---------------------------------------------------------------------------
// EnsureSRV — create/cache an SRV for a texture
// ---------------------------------------------------------------------------
bool PresentHook::EnsureSRV(ID3D11Texture2D* tex,
                              ComPtr<ID3D11ShaderResourceView>& srv)
{
    if (srv) {
        // Validate the cached SRV still refers to this texture
        ComPtr<ID3D11Resource> res;
        srv->GetResource(&res);
        if (res.Get() == tex) return true;
        srv.Reset();
    }
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);

    // Use a UNORM (non-SRGB) view to avoid double gamma-correction on SRGB textures
    DXGI_FORMAT srvFmt = d.Format;
    // Map common SRGB formats to their linear equivalents for sampling
    if (srvFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)  srvFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (srvFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)  srvFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (srvFmt == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB)  srvFmt = DXGI_FORMAT_B8G8R8X8_UNORM;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format              = srvFmt;
    sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(m_dev->CreateShaderResourceView(tex, &sd, &srv))) {
        LOG_ERROR("CreateShaderResourceView failed for tex %p", (void*)tex);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// BlitEye — fullscreen triangle blit of src texture into dstRTV
// ---------------------------------------------------------------------------
void PresentHook::BlitEye(ID3D11Texture2D* src,
                           ID3D11RenderTargetView* dstRTV,
                           uint32_t dstW, uint32_t dstH)
{
    // Build/cache SRV for this source texture
    ComPtr<ID3D11ShaderResourceView>* pCachedSRV = nullptr;
    ComPtr<ID3D11Texture2D>*          pCachedTex = nullptr;
    // We call this for left then right; use a local for simplicity
    ComPtr<ID3D11ShaderResourceView> srvLocal;
    if (!EnsureSRV(src, srvLocal)) return;

    // --- Save minimal state ---
    // Save render targets (we'll restore after blit)
    ComPtr<ID3D11RenderTargetView> savedRTV[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ComPtr<ID3D11DepthStencilView> savedDSV;
    m_ctx->OMGetRenderTargets(1, savedRTV[0].GetAddressOf(), savedDSV.GetAddressOf());

    // Save viewports
    UINT numVP = D3D11_VIEWPORT_AND_SCISSORRECT_MAX_INDEX;
    D3D11_VIEWPORT savedVP[D3D11_VIEWPORT_AND_SCISSORRECT_MAX_INDEX]{};
    m_ctx->RSGetViewports(&numVP, savedVP);

    // Save shaders
    ComPtr<ID3D11VertexShader>  savedVS;
    ComPtr<ID3D11PixelShader>   savedPS;
    ComPtr<ID3D11SamplerState>  savedSmp;
    ComPtr<ID3D11ShaderResourceView> savedSRV;
    ComPtr<ID3D11InputLayout>   savedIL;
    D3D11_PRIMITIVE_TOPOLOGY    savedTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    m_ctx->VSGetShader(&savedVS, nullptr, nullptr);
    m_ctx->PSGetShader(&savedPS, nullptr, nullptr);
    m_ctx->PSGetSamplers(0, 1, savedSmp.GetAddressOf());
    m_ctx->PSGetShaderResources(0, 1, savedSRV.GetAddressOf());
    m_ctx->IAGetInputLayout(&savedIL);
    m_ctx->IAGetPrimitiveTopology(&savedTopo);

    // --- Blit ---
    ID3D11RenderTargetView* rtv = dstRTV;
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp{};
    vp.Width  = static_cast<float>(dstW);
    vp.Height = static_cast<float>(dstH);
    vp.MaxDepth = 1.0f;
    m_ctx->RSSetViewports(1, &vp);

    m_ctx->VSSetShader(m_blitVS.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_blitPS.Get(), nullptr, 0);
    ID3D11SamplerState* smp = m_sampler.Get();
    m_ctx->PSSetSamplers(0, 1, &smp);
    ID3D11ShaderResourceView* srv = srvLocal.Get();
    m_ctx->PSSetShaderResources(0, 1, &srv);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_ctx->Draw(3, 0);

    // --- Restore state ---
    // Unbind SRV before handing back (avoids D3D11 "resource still bound" warnings)
    ID3D11ShaderResourceView* nullSRV = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &nullSRV);
    ID3D11RenderTargetView* nullRTV = nullptr;
    m_ctx->OMSetRenderTargets(1, &nullRTV, nullptr);

    m_ctx->OMSetRenderTargets(1, savedRTV[0].GetAddressOf(), savedDSV.Get());
    if (numVP > 0) m_ctx->RSSetViewports(numVP, savedVP);
    m_ctx->VSSetShader(savedVS.Get(), nullptr, 0);
    m_ctx->PSSetShader(savedPS.Get(), nullptr, 0);
    m_ctx->PSSetSamplers(0, 1, savedSmp.GetAddressOf());
    m_ctx->PSSetShaderResources(0, 1, savedSRV.GetAddressOf());
    m_ctx->IASetInputLayout(savedIL.Get());
    m_ctx->IASetPrimitiveTopology(savedTopo);
}

// ---------------------------------------------------------------------------
// OnPresent — called from hook stub before original Present
// ---------------------------------------------------------------------------
HRESULT PresentHook::OnPresent(IDXGISwapChain* sc, UINT /*syncInterval*/, UINT /*flags*/) {
    if (!m_initialised || !m_stereoHandle) return S_OK;

    // Get pending XR eye textures
    ComPtr<ID3D11Texture2D> pendL, pendR;
    {
        std::lock_guard<std::mutex> lk(m_pendingMtx);
        pendL = m_pendingLeft;
        pendR = m_pendingRight;
        m_pendingLeft.Reset();   // consume — don't re-inject the same frame
        m_pendingRight.Reset();
    }
    if (!pendL || !pendR) return S_OK;

    // Capture game's swap chain on first call (largest/first one that presents)
    if (m_gameSwapChain != sc) {
        DXGI_SWAP_CHAIN_DESC scd{};
        sc->GetDesc(&scd);
        if (scd.BufferDesc.Width > 1) { // ignore our 1×1 vtable-probe dummy (already destroyed)
            m_gameSwapChain = sc;
            m_scW = scd.BufferDesc.Width;
            m_scH = scd.BufferDesc.Height;
            m_backBufRTV.Reset(); // invalidate cached RTV
            LOG_INFO("Captured game swap chain %p (%ux%u fmt=%u)",
                     (void*)sc, m_scW, m_scH, (unsigned)scd.BufferDesc.Format);
        }
    }
    if (m_gameSwapChain != sc) return S_OK;

    // Get/cache RTV for game's back buffer
    if (!m_backBufRTV) {
        ComPtr<ID3D11Texture2D> bb;
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
            LOG_ERROR("OnPresent: GetBuffer(0) failed");
            return S_OK;
        }
        D3D11_TEXTURE2D_DESC bbd{};
        bb->GetDesc(&bbd);
        // Create RTV with explicit UNORM format so we can write to SRGB swap chains too
        DXGI_FORMAT rtvFmt = bbd.Format;
        if (rtvFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) rtvFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (rtvFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) rtvFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
        rtvd.Format        = rtvFmt;
        rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(m_dev->CreateRenderTargetView(bb.Get(), &rtvd, &m_backBufRTV))) {
            LOG_ERROR("OnPresent: CreateRenderTargetView failed (fmt=%u)", (unsigned)rtvFmt);
            return S_OK;
        }
        LOG_VERBOSE("Back buffer RTV created (fmt=%u %ux%u)", (unsigned)rtvFmt, m_scW, m_scH);
    }

    LOG_TRACE("Injecting stereo frame: left=%p right=%p", (void*)pendL.Get(), (void*)pendR.Get());

    // Write left eye: SetActiveEye(LEFT) → blit → left stereo plane
    NvAPI_Stereo_SetActiveEye(m_stereoHandle, NVAPI_STEREO_EYE_LEFT);
    BlitEye(pendL.Get(), m_backBufRTV.Get(), m_scW, m_scH);

    // Write right eye: SetActiveEye(RIGHT) → blit → right stereo plane
    NvAPI_Stereo_SetActiveEye(m_stereoHandle, NVAPI_STEREO_EYE_RIGHT);
    BlitEye(pendR.Get(), m_backBufRTV.Get(), m_scW, m_scH);

    // Reset to mono
    NvAPI_Stereo_SetActiveEye(m_stereoHandle, NVAPI_STEREO_EYE_MONO);

    return S_OK;
}

// ---------------------------------------------------------------------------
// HotkeyThreadProc — 1×1 non-activating window for hotkeys
// ---------------------------------------------------------------------------
void PresentHook::HotkeyThreadProc() {
    static const wchar_t kCls[] = L"XR3DV_Hotkey";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kCls;
    RegisterClassExW(&wc);

    m_hotkeyHwnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kCls, L"", WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!m_hotkeyHwnd) {
        LOG_ERROR("Hotkey window creation failed (GLE=%u)", GetLastError());
        return;
    }

    // Initial press via RegisterHotKey
    RegisterHotKey(m_hotkeyHwnd, HK_SEP_DEC,  MOD_CONTROL | MOD_NOREPEAT, VK_F3);
    RegisterHotKey(m_hotkeyHwnd, HK_SEP_INC,  MOD_CONTROL | MOD_NOREPEAT, VK_F4);
    RegisterHotKey(m_hotkeyHwnd, HK_CONV_DEC, MOD_CONTROL | MOD_NOREPEAT, VK_F5);
    RegisterHotKey(m_hotkeyHwnd, HK_CONV_INC, MOD_CONTROL | MOD_NOREPEAT, VK_F6);
    RegisterHotKey(m_hotkeyHwnd, HK_SAVE,     MOD_CONTROL | MOD_NOREPEAT, VK_F7);

    // TID_HOLD: continuous adjustment while key held (50 ms poll)
    // TID_SYNC: pull sep/conv from 3DV OSD (bidirectional sync)
    SetTimer(m_hotkeyHwnd, TID_HOLD, 50,  nullptr);
    SetTimer(m_hotkeyHwnd, TID_SYNC, 500, nullptr);

    LOG_INFO("Hotkeys ready: Ctrl+F3/F4=separation  Ctrl+F5/F6=convergence  Ctrl+F7=save");

    while (!m_hotkeyStop.load(std::memory_order_relaxed)) {
        MSG msg;
        if (MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT,
                                         MWMO_INPUTAVAILABLE) != WAIT_TIMEOUT) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { m_hotkeyStop = true; goto done; }

                if (msg.message == WM_HOTKEY) {
                    // First press
                    switch (msg.wParam) {
                    case HK_SEP_DEC:  SetSeparation(m_separation   - kSepStep);  break;
                    case HK_SEP_INC:  SetSeparation(m_separation   + kSepStep);  break;
                    case HK_CONV_DEC: SetConvergence(m_convergence - kConvStep); break;
                    case HK_CONV_INC: SetConvergence(m_convergence + kConvStep); break;
                    case HK_SAVE:
                        SaveGameStereoSettings(m_gameIniPath, m_separation, m_convergence);
                        break;
                    }
                }
                else if (msg.message == WM_TIMER) {
                    if (msg.wParam == TID_HOLD) {
                        // Continuous adjustment while held
                        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                        if (ctrl) {
                            if      (GetAsyncKeyState(VK_F3) & 0x8000) SetSeparation(m_separation   - kSepStep);
                            else if (GetAsyncKeyState(VK_F4) & 0x8000) SetSeparation(m_separation   + kSepStep);
                            if      (GetAsyncKeyState(VK_F5) & 0x8000) SetConvergence(m_convergence - kConvStep);
                            else if (GetAsyncKeyState(VK_F6) & 0x8000) SetConvergence(m_convergence + kConvStep);
                        }
                    }
                    else if (msg.wParam == TID_SYNC && m_stereoHandle) {
                        // Pull from 3DV OSD — covers native 3DV hardware button adjustments
                        float dSep = 0.0f, dConv = 0.0f;
                        NvAPI_Stereo_GetSeparation(m_stereoHandle, &dSep);
                        NvAPI_Stereo_GetConvergence(m_stereoHandle, &dConv);
                        if (fabsf(dSep  - m_separation)  > 0.05f) {
                            m_separation  = dSep;
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
    KillTimer(m_hotkeyHwnd, TID_HOLD);
    KillTimer(m_hotkeyHwnd, TID_SYNC);
    for (int i = HK_SEP_DEC; i <= HK_SAVE; ++i)
        UnregisterHotKey(m_hotkeyHwnd, i);
    DestroyWindow(m_hotkeyHwnd);
    m_hotkeyHwnd = nullptr;
}

// ---------------------------------------------------------------------------
// Stereo parameter control — apply to NVAPI immediately
// ---------------------------------------------------------------------------
void PresentHook::SetSeparation(float pct) {
    m_separation = std::max(0.0f, std::min(pct, 100.0f));
    if (m_stereoHandle) NvAPI_Stereo_SetSeparation(m_stereoHandle, m_separation);
    LOG_INFO("Separation: %.1f%%", m_separation);
}

void PresentHook::SetConvergence(float val) {
    m_convergence = std::max(0.0f, std::min(val, 25.0f));
    if (m_stereoHandle) NvAPI_Stereo_SetConvergence(m_stereoHandle, m_convergence);
    LOG_INFO("Convergence: %.3f", m_convergence);
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------
void PresentHook::Shutdown() {
    m_initialised = false;
    m_backBufRTV.Reset();
    m_cachedLeftSRV.Reset(); m_cachedRightSRV.Reset();
    m_cachedLeftTex.Reset(); m_cachedRightTex.Reset();
    {
        std::lock_guard<std::mutex> lk(m_pendingMtx);
        m_pendingLeft.Reset(); m_pendingRight.Reset();
    }
    if (m_stereoHandle) {
        NvAPI_Stereo_DestroyHandle(m_stereoHandle);
        m_stereoHandle = nullptr;
    }
    m_blitVS.Reset(); m_blitPS.Reset(); m_sampler.Reset();
    m_ctx.Reset(); m_dev.Reset();
    NvAPI_Unload();
    // Note: vtable patch intentionally left in place
    // Note: hotkey thread intentionally left running (needed across session reinit)
    LOG_INFO("PresentHook shut down");
}

} // namespace xr3dv
