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
    // Overflow-safe QPC -> nanoseconds.
    // Naive qpc.QuadPart * 1e9 overflows int64 after roughly 9 seconds of
    // ticks at a 1 GHz counter, or ~2.5 hours at a typical 10 MHz QPC.
    // Split into whole seconds + sub-second remainder to stay in range.
    return (qpc.QuadPart / freq.QuadPart) * 1'000'000'000LL
         + (qpc.QuadPart % freq.QuadPart) * 1'000'000'000LL / freq.QuadPart;
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

    // Spin the remaining sub-millisecond gap.
    // Hard 10 ms deadline ensures this can never become an infinite loop
    // if the clock behaves unexpectedly.
    const int64_t spinDeadline = NowNs() + 10'000'000LL;
    while (NowNs() < m_nextDisplayNs) {
        if (NowNs() > spinDeadline) break;
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
