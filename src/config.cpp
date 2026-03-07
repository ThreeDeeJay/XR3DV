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
    return ul.QuadPart / 10000;
}

static auto getUint = [](const IniMap& ini,
                          const std::string& sec, const std::string& key,
                          uint32_t def) -> uint32_t {
    auto sit = ini.find(sec);
    if (sit == ini.end()) return def;
    auto kit = sit->second.find(key);
    if (kit == sit->second.end()) return def;
    try { return static_cast<uint32_t>(std::stoul(kit->second)); } catch (...) { return def; }
};

static auto getFloat = [](const IniMap& ini,
                           const std::string& sec, const std::string& key,
                           float def) -> float {
    auto sit = ini.find(sec);
    if (sit == ini.end()) return def;
    auto kit = sit->second.find(key);
    if (kit == sit->second.end()) return def;
    try { return std::stof(kit->second); } catch (...) { return def; }
};

static auto getString = [](const IniMap& ini,
                            const std::string& sec, const std::string& key,
                            const std::string& def) -> std::string {
    auto sit = ini.find(sec);
    if (sit == ini.end()) return def;
    auto kit = sit->second.find(key);
    if (kit == sit->second.end()) return def;
    return kit->second;
};

static bool getBool(const IniMap& ini,
                    const std::string& sec, const std::string& key, bool def) {
    auto sit = ini.find(sec);
    if (sit == ini.end()) return def;
    auto kit = sit->second.find(key);
    if (kit == sit->second.end()) return def;
    std::string v = ToLower(kit->second);
    return v == "true" || v == "1" || v == "yes";
}

// ---------------------------------------------------------------------------
bool LoadConfig(Config& cfg, const std::string& iniPath) {
    std::lock_guard<std::mutex> lock(cfg.mtx);

    IniMap ini = ParseIni(iniPath);
    cfg.iniPath    = iniPath;
    cfg.iniMtimeMs = FileModTime(iniPath);

    cfg.width       = getUint(ini, "display", "width",       cfg.width);
    cfg.height      = getUint(ini, "display", "height",      cfg.height);
    cfg.monitorRate = getUint(ini, "display", "monitorrate", cfg.monitorRate);

    bool halfRate = getBool(ini, "display", "halfrate", true); // default ON for shutter displays
    cfg.frameRate = halfRate ? cfg.monitorRate / 2 : cfg.monitorRate;

    cfg.separation  = getFloat(ini, "stereo", "separation",  cfg.separation.load());
    cfg.convergence = getFloat(ini, "stereo", "convergence", cfg.convergence.load());
    cfg.ipd         = getFloat(ini, "stereo", "ipd",         cfg.ipd);

    cfg.logLevel = static_cast<int>(getUint(ini, "debug", "loglevel",
                                            static_cast<uint32_t>(cfg.logLevel)));
    cfg.logFile  = getString(ini, "debug", "logfile", cfg.logFile);

    // --- Overlay per-game ini (separation + convergence only) -------------
    if (!cfg.gameIniPath.empty()) {
        IniMap gini = ParseIni(cfg.gameIniPath);
        cfg.gameIniMtimeMs = FileModTime(cfg.gameIniPath);
        cfg.separation  = getFloat(gini, "stereo", "separation",  cfg.separation.load());
        cfg.convergence = getFloat(gini, "stereo", "convergence", cfg.convergence.load());
    }

    // Clamp
    cfg.width       = std::max(320u,  std::min(cfg.width,       7680u));
    cfg.height      = std::max(240u,  std::min(cfg.height,      4320u));
    cfg.monitorRate = std::max(24u,   std::min(cfg.monitorRate,  360u));
    cfg.frameRate   = std::max(24u,   std::min(cfg.frameRate,    360u));
    cfg.separation  = std::max(0.0f,  std::min(cfg.separation.load(),  100.0f));
    cfg.convergence = std::max(0.0f,  std::min(cfg.convergence.load(),  25.0f));
    cfg.ipd         = std::max(0.04f, std::min(cfg.ipd, 0.10f));

    LOG_INFO("Config: %ux%u monitorRate=%uHz frameRate=%uHz sep=%.1f%% conv=%.2f ipd=%.3fm",
             cfg.width, cfg.height, cfg.monitorRate, cfg.frameRate,
             cfg.separation.load(), cfg.convergence.load(), cfg.ipd);
    return true;
}

// ---------------------------------------------------------------------------
bool PollConfigReload(Config& cfg) {
    bool changed = false;
    if (!cfg.iniPath.empty()) {
        uint64_t mt = FileModTime(cfg.iniPath);
        if (mt != 0 && mt != cfg.iniMtimeMs) {
            LOG_VERBOSE("Global config changed -- reloading");
            LoadConfig(cfg, cfg.iniPath);
            changed = true;
        }
    }
    if (!cfg.gameIniPath.empty()) {
        uint64_t mt = FileModTime(cfg.gameIniPath);
        if (mt != 0 && mt != cfg.gameIniMtimeMs) {
            // Reload only sep/conv from game ini
            std::lock_guard<std::mutex> lk(cfg.mtx);
            IniMap gini = ParseIni(cfg.gameIniPath);
            cfg.gameIniMtimeMs = mt;
            cfg.separation  = getFloat(gini, "stereo", "separation",  cfg.separation.load());
            cfg.convergence = getFloat(gini, "stereo", "convergence", cfg.convergence.load());
            cfg.separation  = std::max(0.0f, std::min(cfg.separation.load(),  100.0f));
            cfg.convergence = std::max(0.0f, std::min(cfg.convergence.load(),  25.0f));
            LOG_INFO("Game config reloaded: sep=%.1f%% conv=%.2f",
                     cfg.separation.load(), cfg.convergence.load());
            changed = true;
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
std::string GetDefaultIniPath() {
    char dllPath[MAX_PATH] = {};
    HMODULE hMod = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetDefaultIniPath), &hMod);
    GetModuleFileNameA(hMod, dllPath, MAX_PATH);
    PathRemoveFileSpecA(dllPath);
    return std::string(dllPath) + "\\xr3dv.ini";
}

std::string GetGameIniPath() {
    char exePath[MAX_PATH] = {};
    // GetModuleHandle(nullptr) returns the EXE module from any DLL
    GetModuleFileNameA(GetModuleHandleA(nullptr), exePath, MAX_PATH);
    PathRemoveFileSpecA(exePath);
    return std::string(exePath) + "\\xr3dv.ini";
}

void SaveGameStereoSettings(const std::string& path, float sep, float conv) {
    if (path.empty()) return;

    // Read existing game ini lines, replacing/adding sep+conv under [Stereo]
    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        if (f) {
            std::string l;
            while (std::getline(f, l)) lines.push_back(l);
        }
    }

    // Find/update [Stereo] section
    bool inStereo = false, hasSep = false, hasConv = false;
    for (auto& l : lines) {
        std::string t = ToLower(Trim(l));
        if (!t.empty() && t[0] == '[') {
            inStereo = (t == "[stereo]");
        } else if (inStereo) {
            if (t.rfind("separation", 0) == 0) {
                char buf[64]; snprintf(buf, sizeof(buf), "Separation=%.2f", sep);
                l = buf; hasSep = true;
            } else if (t.rfind("convergence", 0) == 0) {
                char buf[64]; snprintf(buf, sizeof(buf), "Convergence=%.3f", conv);
                l = buf; hasConv = true;
            }
        }
    }

    // Append missing keys
    if (!hasSep || !hasConv) {
        bool foundSection = false;
        for (auto& l : lines) {
            if (ToLower(Trim(l)) == "[stereo]") { foundSection = true; break; }
        }
        if (!foundSection) lines.push_back("[Stereo]");
        char buf[64];
        if (!hasSep)  { snprintf(buf, sizeof(buf), "Separation=%.2f",  sep);  lines.push_back(buf); }
        if (!hasConv) { snprintf(buf, sizeof(buf), "Convergence=%.3f", conv); lines.push_back(buf); }
    }

    std::ofstream f(path);
    if (!f) { LOG_ERROR("SaveGameStereoSettings: cannot write %s", path.c_str()); return; }
    for (auto& l : lines) f << l << "\n";
    LOG_INFO("Saved stereo settings to %s: sep=%.2f conv=%.3f", path.c_str(), sep, conv);
}

} // namespace xr3dv
