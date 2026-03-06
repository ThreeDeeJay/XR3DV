//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdio>
#include <cstdarg>
#include <string>

namespace xr3dv {

enum class LogLevel { Off = 0, Error = 1, Info = 2, Verbose = 3, Trace = 4 };

void LogInit(const std::string& path, LogLevel level);
void LogWrite(LogLevel level, const char* tag, const char* fmt, ...);

} // namespace xr3dv

#define LOG_ERROR(fmt, ...)   ::xr3dv::LogWrite(::xr3dv::LogLevel::Error,   "ERROR",   fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)    ::xr3dv::LogWrite(::xr3dv::LogLevel::Info,    "INFO",    fmt, ##__VA_ARGS__)
#define LOG_VERBOSE(fmt, ...) ::xr3dv::LogWrite(::xr3dv::LogLevel::Verbose, "VERBOSE", fmt, ##__VA_ARGS__)
#define LOG_TRACE(fmt, ...)   ::xr3dv::LogWrite(::xr3dv::LogLevel::Trace,   "TRACE",   fmt, ##__VA_ARGS__)
