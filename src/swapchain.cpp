//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#include "pch.h"
#include "swapchain.h"
#include "logging.h"

namespace xr3dv {

// Returns true for any DXGI format that requires D3D11_BIND_DEPTH_STENCIL.
static bool IsDepthFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
        default:
            return false;
    }
}

bool Swapchain::Init(ID3D11Device* dev, const XrSwapchainCreateInfo& ci) {
    m_width   = ci.width;
    m_height  = ci.height;
    m_format  = static_cast<uint32_t>(ci.format);
    m_isDepth = IsDepthFormat(static_cast<DXGI_FORMAT>(ci.format));

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width          = ci.width;
    desc.Height         = ci.height;
    desc.MipLevels      = (ci.mipCount  > 0) ? ci.mipCount  : 1;
    desc.ArraySize      = (ci.arraySize > 0) ? ci.arraySize : 1;
    desc.Format         = static_cast<DXGI_FORMAT>(ci.format);
    desc.SampleDesc     = {ci.sampleCount > 0 ? ci.sampleCount : 1, 0};
    desc.Usage          = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags      = 0;

    if (m_isDepth) {
        // Depth formats must use BIND_DEPTH_STENCIL; BIND_RENDER_TARGET or
        // BIND_SHADER_RESOURCE with a plain depth format is E_INVALIDARG.
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    } else {
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    }

    m_images.resize(kImageCount);
    for (uint32_t i = 0; i < kImageCount; ++i) {
        HRESULT hr = dev->CreateTexture2D(&desc, nullptr, &m_images[i].tex);
        if (FAILED(hr)) {
            LOG_ERROR("Swapchain: CreateTexture2D [%u] fmt=%u depth=%d failed: 0x%08X",
                      i, m_format, (int)m_isDepth, (unsigned)hr);
            return false;
        }

        // SRV is only valid for colour textures; depth textures need a
        // typeless base format (which we don't use), so leave srv null.
        if (!m_isDepth) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format                    = desc.Format;
            srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels       = desc.MipLevels;

            hr = dev->CreateShaderResourceView(m_images[i].tex.Get(), &srvDesc,
                                                &m_images[i].srv);
            if (FAILED(hr)) {
                LOG_ERROR("Swapchain: CreateSRV [%u] failed: 0x%08X", i, (unsigned)hr);
                return false;
            }
        }

        m_freeQueue.push(i);
    }

    LOG_VERBOSE("Swapchain created: %ux%u fmt=%u %s count=%u",
                m_width, m_height, m_format,
                m_isDepth ? "DEPTH" : "COLOR", kImageCount);
    return true;
}

bool Swapchain::InitShared(ID3D11Device* d3d11Dev, ID3D12Device* d3d12Dev,
                            const XrSwapchainCreateInfo& ci)
{
    m_width    = ci.width;
    m_height   = ci.height;
    m_format   = static_cast<uint32_t>(ci.format);
    m_isDepth  = IsDepthFormat(static_cast<DXGI_FORMAT>(ci.format));
    m_isShared = true;

    // D3D12-shared textures must use NT shared handles.
    // SHARED_NTHANDLE requires D3D11.1+ and a compatible format.
    // Depth formats are not shareable this way — create plain D3D11 textures
    // for depth (the app won't sample them cross-API anyway).
    if (m_isDepth) {
        LOG_VERBOSE("Swapchain (shared/depth): falling back to plain D3D11 for depth format");
        return Init(d3d11Dev, ci);
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width          = ci.width;
    desc.Height         = ci.height;
    desc.MipLevels      = (ci.mipCount  > 0) ? ci.mipCount  : 1;
    desc.ArraySize      = (ci.arraySize > 0) ? ci.arraySize : 1;
    desc.Format         = static_cast<DXGI_FORMAT>(ci.format);
    desc.SampleDesc     = {ci.sampleCount > 0 ? ci.sampleCount : 1, 0};
    desc.Usage          = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // NT shared handle allows cross-adapter/cross-API sharing.
    // SHARED_KEYEDMUTEX not used — D3D12 fence handles sync on that side.
    desc.MiscFlags      = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                          D3D11_RESOURCE_MISC_SHARED;

    m_images.resize(kImageCount);
    for (uint32_t i = 0; i < kImageCount; ++i) {
        HRESULT hr = d3d11Dev->CreateTexture2D(&desc, nullptr, &m_images[i].tex);
        if (FAILED(hr)) {
            LOG_ERROR("Swapchain (shared): CreateTexture2D [%u] failed: 0x%08X", i, (unsigned)hr);
            return false;
        }

        // Create D3D11 SRV for our presentation readback path
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format                    = desc.Format;
            srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels       = desc.MipLevels;
            hr = d3d11Dev->CreateShaderResourceView(m_images[i].tex.Get(), &srvDesc,
                                                     &m_images[i].srv);
            if (FAILED(hr)) {
                LOG_ERROR("Swapchain (shared): CreateSRV [%u] failed: 0x%08X", i, (unsigned)hr);
                return false;
            }
        }

        // Export NT shared handle from D3D11 texture
        Microsoft::WRL::ComPtr<IDXGIResource1> dxgiRes;
        hr = m_images[i].tex.As(&dxgiRes);
        if (FAILED(hr)) {
            LOG_ERROR("Swapchain (shared): QI IDXGIResource1 [%u] failed: 0x%08X", i, (unsigned)hr);
            return false;
        }
        HANDLE sharedHandle = nullptr;
        hr = dxgiRes->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedHandle);
        if (FAILED(hr)) {
            LOG_ERROR("Swapchain (shared): CreateSharedHandle [%u] failed: 0x%08X", i, (unsigned)hr);
            return false;
        }

        // Open shared handle in D3D12 — pure COM vtable, no d3d12.lib needed
        hr = d3d12Dev->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&m_images[i].d3d12Tex));
        CloseHandle(sharedHandle); // D3D12 holds its own ref; close ours
        if (FAILED(hr)) {
            LOG_ERROR("Swapchain (shared): OpenSharedHandle [%u] in D3D12 failed: 0x%08X",
                      i, (unsigned)hr);
            return false;
        }

        m_freeQueue.push(i);
    }

    LOG_VERBOSE("Swapchain (shared D3D11+D3D12): %ux%u fmt=%u count=%u",
                m_width, m_height, m_format, kImageCount);
    return true;
}


XrResult Swapchain::Acquire(uint32_t& outIndex) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_acquired >= 0)
        return XR_ERROR_CALL_ORDER_INVALID;
    if (m_freeQueue.empty())
        return XR_ERROR_CALL_ORDER_INVALID;

    outIndex   = m_freeQueue.front();
    m_freeQueue.pop();
    m_acquired = static_cast<int32_t>(outIndex);
    return XR_SUCCESS;
}

XrResult Swapchain::Wait(const XrSwapchainImageWaitInfo& /*info*/) {
    // D3D11 resources are immediately ready — no GPU fence wait needed here.
    // A full implementation would signal a fence when the previous display
    // of this image has completed.
    return XR_SUCCESS;
}

XrResult Swapchain::Release() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_acquired < 0)
        return XR_ERROR_CALL_ORDER_INVALID;

    m_latest   = static_cast<uint32_t>(m_acquired);
    m_freeQueue.push(static_cast<uint32_t>(m_acquired));
    m_acquired = -1;
    return XR_SUCCESS;
}

const Swapchain::Image* Swapchain::GetLatestImage() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return &m_images[m_latest];
}

} // namespace xr3dv
