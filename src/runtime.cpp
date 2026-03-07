//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#include "pch.h"
#include "runtime.h"
#include "logging.h"

namespace xr3dv {

static Runtime* g_runtime = nullptr;

Runtime* GetRuntime() { return g_runtime; }

bool RuntimeInit(Runtime** out) {
    if (g_runtime) {
        if (out) *out = g_runtime;
        return true; // already initialised
    }

    auto* rt = new Runtime();

    // Load config — set game ini path first so it overlays sep/conv at startup
    std::string iniPath = GetDefaultIniPath();
    std::string gameIniPath = GetGameIniPath();
    // Only use game ini if it's a different directory than the global ini
    if (gameIniPath != iniPath)
        rt->cfg.gameIniPath = gameIniPath;
    LoadConfig(rt->cfg, iniPath);

    // Initialise logging
    LogInit(rt->cfg.logFile, static_cast<LogLevel>(rt->cfg.logLevel));

    g_runtime = rt;
    if (out) *out = rt;
    LOG_INFO("XR3DV runtime initialised (v%d.%d.%d+%s)",
             XR3DV_VERSION_MAJOR, XR3DV_VERSION_MINOR, XR3DV_VERSION_PATCH,
             XR3DV_GIT_HASH);
    return true;
}

void RuntimeDestroy() {
    delete g_runtime;
    g_runtime = nullptr;
}

} // namespace xr3dv
