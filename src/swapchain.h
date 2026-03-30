//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <openxr/openxr.h>
#include <d3d11.h>
#include <dxgi1_2.h>      // IDXGIResource1::CreateSharedHandle
#include <wrl/client.h>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>

namespace xr3dv {

// Forward-declare to avoid pulling d3d12.h into every translation unit.
// swapchain.cpp includes d3d12.h directly.
struct ID3D12Device;

/// A pool of textures exposed as an XrSwapchain.
/// D3D11 mode: plain D3D11 textures (app renders via D3D11 device).
/// D3D12 mode: D3D11 textures shared with the app's D3D12 device via NT
///             shared handles (app renders into ID3D12Resource; we read back
///             via the paired D3D11 SRVs).
class Swapchain {
public:
    static constexpr uint32_t kImageCount = 3;

    struct Image {
        Microsoft::WRL::ComPtr<ID3D11Texture2D>          tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        void* d3d12Tex = nullptr; // ID3D12Resource* (AddRef'd), D3D12 mode only
    };

    Swapchain() = default;
    ~Swapchain();

    /// Allocate plain D3D11 textures (app uses D3D11).
    bool Init(ID3D11Device* dev, const XrSwapchainCreateInfo& ci);

    /// Allocate D3D11 textures shared with a D3D12 device via NT handles.
    bool InitShared(ID3D11Device* d3d11Dev, ID3D12Device* d3d12Dev,
                    const XrSwapchainCreateInfo& ci);

    // OpenXR swapchain operations
    XrResult Acquire(uint32_t& outIndex);
    XrResult Wait(const XrSwapchainImageWaitInfo& info);
    XrResult Release();

    uint32_t Width()    const { return m_width; }
    uint32_t Height()   const { return m_height; }
    uint32_t Format()   const { return m_format; }
    bool     IsDepth()  const { return m_isDepth; }
    bool     IsShared() const { return m_isShared; }

    const Image* GetLatestImage() const;
    std::vector<Image>& Images() { return m_images; }

private:
    std::vector<Image>   m_images;
    std::queue<uint32_t> m_freeQueue;
    mutable std::mutex   m_mtx;

    int32_t  m_acquired  = -1;
    uint32_t m_latest    = 0;
    uint32_t m_width     = 0;
    uint32_t m_height    = 0;
    uint32_t m_format    = 0;
    bool     m_isDepth   = false;
    bool     m_isShared  = false;
};

} // namespace xr3dv
