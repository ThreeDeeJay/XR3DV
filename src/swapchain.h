//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <openxr/openxr.h>
#include <d3d11.h>
#include <d3d12.h>        // ID3D12Device, ID3D12Resource — COM vtable only, no d3d12.lib needed
#include <dxgi1_2.h>      // IDXGIResource1::CreateSharedHandle
#include <wrl/client.h>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>

namespace xr3dv {

/// A pool of textures exposed as an XrSwapchain.
/// D3D11 mode: plain D3D11 textures (app renders via D3D11 device).
/// D3D12 mode: D3D11 textures with NT shared handles opened as D3D12 resources
///             (app renders via D3D12; we read back via D3D11 SRVs).
class Swapchain {
public:
    static constexpr uint32_t kImageCount = 3;

    struct Image {
        Microsoft::WRL::ComPtr<ID3D11Texture2D>          tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D12Resource>           d3d12Tex; // D3D12 mode only
    };

    Swapchain() = default;
    ~Swapchain() = default;

    /// Allocate plain D3D11 textures (app uses D3D11).
    bool Init(ID3D11Device* dev, const XrSwapchainCreateInfo& ci);

    /// Allocate D3D11 textures shared with a D3D12 device via NT handles.
    /// d3d12Dev is used only to open the shared handle — no d3d12.lib required.
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

    /// Get the most-recently-released image (the one to display).
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
    bool     m_isShared  = false; // true = D3D12 shared mode
};

} // namespace xr3dv
