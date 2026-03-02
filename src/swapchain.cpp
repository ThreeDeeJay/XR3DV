//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#include "swapchain.h"
#include "logging.h"

namespace xr3dv {

bool Swapchain::Init(ID3D11Device* dev, const XrSwapchainCreateInfo& ci) {
    m_width  = ci.width;
    m_height = ci.height;
    m_format = ci.format; // DXGI_FORMAT cast

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width          = ci.width;
    desc.Height         = ci.height;
    desc.MipLevels      = (ci.mipCount > 0) ? ci.mipCount : 1;
    desc.ArraySize      = (ci.arraySize > 0) ? ci.arraySize : 1;
    desc.Format         = static_cast<DXGI_FORMAT>(ci.format);
    desc.SampleDesc     = {ci.sampleCount > 0 ? ci.sampleCount : 1, 0};
    desc.Usage          = D3D11_USAGE_DEFAULT;
    desc.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags      = 0;

    m_images.resize(kImageCount);
    for (uint32_t i = 0; i < kImageCount; ++i) {
        HRESULT hr = dev->CreateTexture2D(&desc, nullptr, &m_images[i].tex);
        if (FAILED(hr)) {
            LOG_ERROR("Swapchain: CreateTexture2D [%u] failed: 0x%08X", i, hr);
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                    = desc.Format;
        srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels       = desc.MipLevels;

        hr = dev->CreateShaderResourceView(m_images[i].tex.Get(), &srvDesc,
                                            &m_images[i].srv);
        if (FAILED(hr)) {
            LOG_ERROR("Swapchain: CreateSRV [%u] failed: 0x%08X", i, hr);
            return false;
        }

        m_freeQueue.push(i);
    }

    LOG_VERBOSE("Swapchain created: %ux%u fmt=%u count=%u",
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
