//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <openxr/openxr.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>

namespace xr3dv {

/// A pool of D3D11 Texture2D objects exposed as an XrSwapchain.
/// Supports double/triple-buffering via acquire/release semantics.
class Swapchain {
public:
    static constexpr uint32_t kImageCount = 3;

    struct Image {
        Microsoft::WRL::ComPtr<ID3D11Texture2D>          tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };

    Swapchain() = default;
    ~Swapchain() = default;

    /// Allocate D3D11 textures.
    bool Init(ID3D11Device* dev, const XrSwapchainCreateInfo& ci);

    // OpenXR swapchain operations
    XrResult Acquire(uint32_t& outIndex);
    XrResult Wait(const XrSwapchainImageWaitInfo& info);
    XrResult Release();

    uint32_t Width()   const { return m_width; }
    uint32_t Height()  const { return m_height; }
    uint32_t Format()  const { return m_format; }
    bool     IsDepth() const { return m_isDepth; }

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
    uint32_t m_format    = 0;   // DXGI_FORMAT
    bool     m_isDepth   = false;
};

} // namespace xr3dv
