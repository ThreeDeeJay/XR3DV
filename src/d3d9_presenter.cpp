//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  Fast-path D3D11 → D3D9 blit using DXGI shared surface handles.
//  This avoids the CPU readback in nvapi_stereo.cpp at the cost of needing
//  the D3D11 texture to have been created with D3D11_RESOURCE_MISC_SHARED.
//
//  Usage: If the OpenXR application creates its swapchain with
//         XrSwapchainCreateInfo::usageFlags containing XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
//         and our Swapchain::Init adds D3D11_RESOURCE_MISC_SHARED, we can
//         open the shared handle in D3D9 and StretchRect GPU-to-GPU.

#include "d3d9_presenter.h"
#include "logging.h"

#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace xr3dv {

/// Try to open a D3D11 texture as a shared D3D9 surface.
/// Returns true on success and populates `sharedSurface`.
bool OpenSharedD3D11TextureAsD3D9Surface(
    ID3D11Texture2D*     tex,
    IDirect3DDevice9Ex*  d3d9Dev,
    IDirect3DTexture9**  outTex)
{
    // Obtain DXGI resource interface to get the shared handle
    ComPtr<IDXGIResource> dxgiRes;
    if (FAILED(tex->QueryInterface(IID_PPV_ARGS(&dxgiRes)))) {
        LOG_VERBOSE("OpenShared: texture does not support IDXGIResource");
        return false;
    }

    HANDLE sharedHandle = nullptr;
    if (FAILED(dxgiRes->GetSharedHandle(&sharedHandle)) || !sharedHandle) {
        LOG_VERBOSE("OpenShared: GetSharedHandle failed (texture not shared?)");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);

    HRESULT hr = d3d9Dev->CreateTexture(
        desc.Width, desc.Height, 1,
        D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8,   // must match D3D11 format
        D3DPOOL_DEFAULT,
        outTex,
        &sharedHandle);

    if (FAILED(hr)) {
        LOG_VERBOSE("OpenShared: CreateTexture from shared handle failed: 0x%08X", hr);
        return false;
    }

    LOG_VERBOSE("OpenShared: GPU-shared handle opened successfully");
    return true;
}

} // namespace xr3dv
