//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace xr3dv {

/// Runtime-wide configuration.  Loaded from xr3dv.ini next to the DLL.
/// Hot-reloadable fields (separation / convergence) are atomic.
struct Config {
    // ---- [Display] -------------------------------------------------------
    uint32_t width      = 1920;   ///< Per-eye render width  (pixels)
    uint32_t height     = 1080;   ///< Per-eye render height (pixels)
    uint32_t frameRate  = 120;    ///< Target display refresh (Hz)

    // ---- [Stereo] --------------------------------------------------------
    /// Eye separation as a percentage [0, 100].
    /// Mapped to NvAPI NvStereoActivationFlag via NvAPI_Stereo_SetSeparation.
    std::atomic<float> separation { 50.0f };

    /// Convergence point [0, 25].
    std::atomic<float> convergence { 5.0f };

    /// Interpupillary distance in metres (used for view pose offsets).
    float ipd = 0.064f;

    // ---- [Debug] ---------------------------------------------------------
    int         logLevel = 4;    ///< 0=off 1=errors 2=info 3=verbose 4=trace
    std::string logFile  = "xr3dv.log";

    // ---- Internal --------------------------------------------------------
    std::string iniPath;          ///< Absolute path of the INI file
    uint64_t    iniMtimeMs = 0;   ///< Last modification time (ms since epoch)
    mutable std::mutex mtx;       ///< Protects non-atomic fields during reload

    Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
};

/// Load config from path.  Returns true on success.
bool LoadConfig(Config& cfg, const std::string& iniPath);

/// Poll the INI file; reload if modified.  Call periodically (every ~500 ms).
void PollConfigReload(Config& cfg);

/// Returns the absolute path of the INI file next to the DLL.
std::string GetDefaultIniPath();

} // namespace xr3dv
