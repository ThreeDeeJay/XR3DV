//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#include "pch.h"
#include "timing.h"

namespace xr3dv {

FrameTimer::FrameTimer(uint32_t targetHz) {
    QueryPerformanceFrequency(&m_freq);
    QueryPerformanceCounter(&m_lastQpc);
    SetTargetHz(targetHz);
}

void FrameTimer::SetTargetHz(uint32_t hz) {
    if (hz == 0) hz = 60;
    m_periodNs = 1'000'000'000LL / hz;
}

int64_t FrameTimer::NowNs() {
    LARGE_INTEGER freq, qpc;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&qpc);
    return (qpc.QuadPart * 1'000'000'000LL) / freq.QuadPart;
}

int64_t FrameTimer::WaitAndGetNextDisplayTime() {
    int64_t now = NowNs();

    if (m_first) {
        m_first         = false;
        m_nextDisplayNs = now + m_periodNs;
        return m_nextDisplayNs;
    }

    // Sleep until the next vsync slot
    int64_t sleepNs = m_nextDisplayNs - now - 500'000LL; // 0.5 ms margin
    if (sleepNs > 1'000'000LL) {
        DWORD sleepMs = static_cast<DWORD>(sleepNs / 1'000'000LL);
        Sleep(sleepMs);
    }

    // Spin the remaining sub-millisecond gap
    while (NowNs() < m_nextDisplayNs) {
        _mm_pause(); // avoid CPU thrashing (include <immintrin.h> or use YieldProcessor)
        YieldProcessor();
    }

    int64_t displayTime = m_nextDisplayNs;
    m_nextDisplayNs    += m_periodNs;

    // Drift correction: if we're more than one period behind, catch up.
    now = NowNs();
    while (m_nextDisplayNs < now) m_nextDisplayNs += m_periodNs;

    return displayTime;
}

} // namespace xr3dv
