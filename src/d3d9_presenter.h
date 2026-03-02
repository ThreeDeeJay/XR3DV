//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <d3d9.h>
#include <d3d11.h>

namespace xr3dv {
bool OpenSharedD3D11TextureAsD3D9Surface(
    ID3D11Texture2D*    tex,
    IDirect3DDevice9Ex* d3d9Dev,
    IDirect3DTexture9** outTex);
} // namespace xr3dv
