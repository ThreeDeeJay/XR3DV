//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  Precompiled header.
//  Rule: d3d11.h (and the XR_USE_GRAPHICS_API_D3D11 guard) MUST be seen by
//  the compiler BEFORE openxr_platform.h in every translation unit.
//  Including this header first in every .cpp guarantees that ordering.

#pragma once

// ---- Windows / MSVC boilerplate -----------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

// ---- DirectX (order matters) --------------------------------------------
#include <d3d9.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

// ---- OpenXR: define the D3D11 platform flag BEFORE including the header -
#ifndef XR_USE_GRAPHICS_API_D3D11
#  define XR_USE_GRAPHICS_API_D3D11
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

// ---- Standard library ---------------------------------------------------
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
