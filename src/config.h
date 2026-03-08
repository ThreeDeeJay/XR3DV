//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace xr3dv {

/// Runtime-wide configuration.  Loaded from xr3dv.ini next to the DLL,
/// then overlaid with the per-game xr3dv.ini next to the game exe.
struct Config {
    // ---- [Display] -------------------------------------------------------
    uint32_t width       = 1920;  ///< Per-eye render width  (pixels)
    uint32_t height      = 1080;  ///< Per-eye render height (pixels)

    /// Actual monitor refresh rate (Hz).  Used for D3D9 FSE device creation.
    uint32_t monitorRate = 120;

    /// Frame rate exposed to xrWaitFrame.
    /// When HalfRate=true (default): monitorRate/2  (60 fps for a 120 Hz shutter display).
    /// When HalfRate=false: same as monitorRate.
    uint32_t frameRate   = 60;

    // ---- [Stereo] --------------------------------------------------------
    /// Eye separation as a percentage [0, 100].
    std::atomic<float> separation  { 50.0f };
    /// Convergence point [0, 25].
    std::atomic<float> convergence { 5.0f  };
    /// Interpupillary distance in metres (used for view pose offsets).
    float ipd = 0.064f;
    /// Swap left and right eyes if they appear reversed.
    bool swapEyes = false;

    // ---- [Debug] ---------------------------------------------------------
    int         logLevel = 1;    ///< 0=off 1=errors 2=info 3=verbose 4=trace
    std::string logFile  = "xr3dv.log";

    // ---- Internal --------------------------------------------------------
    std::string iniPath;          ///< Absolute path of the global INI file
    std::string gameIniPath;      ///< Absolute path of the per-game INI file
    uint64_t    iniMtimeMs     = 0;
    uint64_t    gameIniMtimeMs = 0;
    mutable std::mutex mtx;

    Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
};

/// Load config from path.  Returns true on success.
bool LoadConfig(Config& cfg, const std::string& iniPath);

/// Poll INI files; reload stereo params if modified.  Returns true if changed.
bool PollConfigReload(Config& cfg);

/// Returns the absolute path of the global INI file (next to the DLL).
std::string GetDefaultIniPath();

/// Returns the path next to the game executable (xr3dv.ini in exe folder).
/// Returns "" if it cannot be determined.
std::string GetGameIniPath();

/// Write separation and convergence to the per-game INI (creating it if needed).
void SaveGameStereoSettings(const std::string& gameIniPath, float sep, float conv);

} // namespace xr3dv
