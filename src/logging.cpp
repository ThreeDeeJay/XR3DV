//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#include "pch.h"
#include "logging.h"

namespace xr3dv {

static std::mutex  g_logMtx;
static std::string g_logPath;
static LogLevel    g_logLevel = LogLevel::Error;
static std::ofstream g_logFile;

void LogInit(const std::string& path, LogLevel level) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    g_logLevel = level;
    g_logPath  = path;
    if (level != LogLevel::Off) {
        g_logFile.open(path, std::ios::app);
    }
}

void LogWrite(LogLevel level, const char* fmt, ...) {
    if (level > g_logLevel) return;

    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[64];
    snprintf(ts, sizeof(ts), "[%02d:%02d:%02d.%03d] ",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::lock_guard<std::mutex> lk(g_logMtx);
    if (g_logFile.is_open()) {
        g_logFile << ts << buf << '\n';
        g_logFile.flush();
    }
    OutputDebugStringA((std::string("XR3DV: ") + ts + buf + "\n").c_str());
}

} // namespace xr3dv
