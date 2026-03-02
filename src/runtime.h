//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "config.h"
#include "session.h"

namespace xr3dv {

/// Singleton global runtime state.
struct Runtime {
    Config   cfg;
    std::mutex sessionMtx;
    std::unordered_map<uint64_t, std::unique_ptr<Session>> sessions;
    uint64_t nextSessionHandle = 1;

    Session* GetSession(XrSession s) {
        std::lock_guard<std::mutex> lk(sessionMtx);
        auto it = sessions.find(reinterpret_cast<uint64_t>(s));
        return (it == sessions.end()) ? nullptr : it->second.get();
    }
};

/// Access the single Runtime instance (created when xrCreateInstance succeeds).
Runtime* GetRuntime();

/// Called by xrCreateInstance / xrDestroyInstance.
bool RuntimeInit(Runtime** out);
void RuntimeDestroy();

} // namespace xr3dv
