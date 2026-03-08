//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  PresentHook: hooks IDXGISwapChain::Present on the game's own swap chain,
//  then injects the XR stereo frame via NVAPI stereo on the game's D3D11 device.
//  No separate window. No focus stealing. No audio disruption.

#pragma once
#include <dxgi1_2.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <nvapi.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <cstdint>

namespace xr3dv {

class PresentHook {
public:
    static PresentHook& Get();
    ~PresentHook();
    PresentHook(const PresentHook&) = delete;
    PresentHook& operator=(const PresentHook&) = delete;

    // Called from Session::InitD3D11 — hooks Present vtable and inits NVAPI
    bool InitStereo(ID3D11Device* dev,
                    float sep, float conv,
                    const std::string& gameIniPath);

    // Called from xrEndFrame — stores XR left/right eye textures for next Present
    void SetPendingFrame(ID3D11ShaderResourceView* leftSRV,
                         ID3D11ShaderResourceView* rightSRV);

    // Called from Session destructor
    void Shutdown();

    void  SetSeparation(float pct);
    void  SetConvergence(float val);
    float GetSeparation()  const { return m_separation;  }
    float GetConvergence() const { return m_convergence; }
    bool  IsInitialised()  const { return m_initialised; }

    // Called by the static hook stub — must be public
    HRESULT OnPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags);

private:
    PresentHook() = default;

    // --- DXGI vtable hook ---
    bool HookPresent(ID3D11Device* dev);
    static void PatchVTable(void* obj, size_t idx, void* hook, void** orig);

    // --- Frame injection ---
    void InjectStereoFrame(IDXGISwapChain* sc);
    void BlitEye(ID3D11Texture2D* src, ID3D11RenderTargetView* dstRTV,
                 uint32_t dstW, uint32_t dstH);
    bool EnsureBlitShaders();
    bool EnsureSRV(ID3D11Texture2D* tex,
                   Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv);

    // --- Hotkey thread ---
    void HotkeyThreadProc();

    // --- D3D11 blit resources ---
    Microsoft::WRL::ComPtr<ID3D11Device>        m_dev;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>  m_blitVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>   m_blitPS;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>  m_sampler;
    // Cached SRVs for pending textures
    Microsoft::WRL::ComPtr<ID3D11Texture2D>            m_cachedLeftTex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>            m_cachedRightTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>   m_cachedLeftSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>   m_cachedRightSRV;

    // --- NVAPI ---
    StereoHandle m_stereoHandle = nullptr;

    // --- Pending stereo frame ---
    std::mutex                                       m_pendingMtx;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_pendingLeft;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_pendingRight;

    // --- Swap chain capture ---
    IDXGISwapChain* m_gameSwapChain = nullptr; // weak ref — game owns it
    uint32_t        m_scW = 0, m_scH = 0;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_backBufRTV;

    // --- Hotkey thread ---
    std::thread       m_hotkeyThread;
    std::atomic<bool> m_hotkeyStop{false};
    HWND              m_hotkeyHwnd = nullptr;

    static constexpr int      HK_SEP_DEC  = 1;  // Ctrl+F3
    static constexpr int      HK_SEP_INC  = 2;  // Ctrl+F4
    static constexpr int      HK_CONV_DEC = 3;  // Ctrl+F5
    static constexpr int      HK_CONV_INC = 4;  // Ctrl+F6
    static constexpr int      HK_SAVE     = 5;  // Ctrl+F7
    static constexpr UINT_PTR TID_HOLD    = 1;  // 50 ms hold detection
    static constexpr UINT_PTR TID_SYNC    = 2;  // 500 ms 3DV OSD sync
    static constexpr float    kSepStep    = 1.0f;
    static constexpr float    kConvStep   = 0.1f;

    // --- State ---
    bool        m_initialised   = false;
    bool        m_presentHooked = false;
    float       m_separation    = 50.0f;
    float       m_convergence   = 5.0f;
    std::string m_gameIniPath;
};

} // namespace xr3dv
