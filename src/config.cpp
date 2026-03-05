//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#include "pch.h"
#include "config.h"
#include "logging.h"

namespace xr3dv {

// ---------------------------------------------------------------------------
// Tiny INI parser
// ---------------------------------------------------------------------------
using IniMap = std::unordered_map<std::string, std::unordered_map<std::string, std::string>>;

static std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return s;
}

static IniMap ParseIni(const std::string& path) {
    IniMap result;
    std::ifstream f(path);
    if (!f) return result;

    std::string line, section;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            size_t end = line.find(']');
            if (end != std::string::npos)
                section = ToLower(Trim(line.substr(1, end - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = ToLower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));
        // Strip inline comment
        size_t sc = val.find(';');
        if (sc != std::string::npos) val = Trim(val.substr(0, sc));
        result[section][key] = val;
    }
    return result;
}

static uint64_t FileModTime(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    ULARGE_INTEGER ul;
    ul.LowPart  = d.ftLastWriteTime.dwLowDateTime;
    ul.HighPart = d.ftLastWriteTime.dwHighDateTime;
    return ul.QuadPart / 10000; // 100-ns units → ms
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool LoadConfig(Config& cfg, const std::string& iniPath) {
    std::lock_guard<std::mutex> lock(cfg.mtx);

    IniMap ini = ParseIni(iniPath);
    cfg.iniPath    = iniPath;
    cfg.iniMtimeMs = FileModTime(iniPath);

    auto getUint = [&](const std::string& sec, const std::string& key, uint32_t def) -> uint32_t {
        auto sit = ini.find(sec);
        if (sit == ini.end()) return def;
        auto kit = sit->second.find(key);
        if (kit == sit->second.end()) return def;
        try { return static_cast<uint32_t>(std::stoul(kit->second)); }
        catch (...) { return def; }
    };
    auto getFloat = [&](const std::string& sec, const std::string& key, float def) -> float {
        auto sit = ini.find(sec);
        if (sit == ini.end()) return def;
        auto kit = sit->second.find(key);
        if (kit == sit->second.end()) return def;
        try { return std::stof(kit->second); }
        catch (...) { return def; }
    };
    auto getString = [&](const std::string& sec, const std::string& key, const std::string& def) -> std::string {
        auto sit = ini.find(sec);
        if (sit == ini.end()) return def;
        auto kit = sit->second.find(key);
        if (kit == sit->second.end()) return def;
        return kit->second;
    };

    cfg.width      = getUint("display",  "width",     cfg.width);
    cfg.height     = getUint("display",  "height",    cfg.height);
    cfg.frameRate  = getUint("display",  "framerate", cfg.frameRate);

    cfg.separation  = getFloat("stereo", "separation",  cfg.separation.load());
    cfg.convergence = getFloat("stereo", "convergence", cfg.convergence.load());
    cfg.ipd         = getFloat("stereo", "ipd",         cfg.ipd);

    cfg.logLevel = static_cast<int>(getUint("debug", "loglevel", static_cast<uint32_t>(cfg.logLevel)));
    cfg.logFile  = getString("debug", "logfile", cfg.logFile);

    // Clamp
    cfg.width     = std::max(320u,  std::min(cfg.width,     7680u));
    cfg.height    = std::max(240u,  std::min(cfg.height,    4320u));
    cfg.frameRate = std::max(24u,   std::min(cfg.frameRate, 240u));
    cfg.separation  = std::max(0.0f, std::min(cfg.separation.load(),  100.0f));
    cfg.convergence = std::max(0.0f, std::min(cfg.convergence.load(),  25.0f));
    cfg.ipd         = std::max(0.04f, std::min(cfg.ipd, 0.10f));

    LOG_INFO("Config loaded: %ux%u @%uHz  sep=%.1f%%  conv=%.2f  ipd=%.3fm",
             cfg.width, cfg.height, cfg.frameRate,
             cfg.separation.load(), cfg.convergence.load(), cfg.ipd);
    return true;
}

void PollConfigReload(Config& cfg) {
    if (cfg.iniPath.empty()) return;
    uint64_t mtime = FileModTime(cfg.iniPath);
    if (mtime == 0 || mtime == cfg.iniMtimeMs) return;

    LOG_VERBOSE("Config file changed — reloading...");
    LoadConfig(cfg, cfg.iniPath);
}

std::string GetDefaultIniPath() {
    char dllPath[MAX_PATH] = {};
    HMODULE hMod = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetDefaultIniPath),
        &hMod);
    GetModuleFileNameA(hMod, dllPath, MAX_PATH);
    PathRemoveFileSpecA(dllPath);
    return std::string(dllPath) + "\\xr3dv.ini";
}

} // namespace xr3dv
