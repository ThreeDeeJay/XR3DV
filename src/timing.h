//  XR3DV - OpenXR Runtime for NVIDIA 3D Vision
//  Copyright (C) 2026 XR3DV Contributors
//  SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>
#include <windows.h>

namespace xr3dv {

/// High-resolution frame timer driven by QPC.
class FrameTimer {
public:
    explicit FrameTimer(uint32_t targetHz = 120);

    /// Return the XrTime (nanoseconds from epoch) for the next predicted
    /// display time.  Blocks if needed to maintain the target frame rate.
    int64_t WaitAndGetNextDisplayTime();

    /// Current QPC time in nanoseconds (OpenXR epoch).
    static int64_t NowNs();

    void SetTargetHz(uint32_t hz);

private:
    LARGE_INTEGER m_freq;
    LARGE_INTEGER m_lastQpc;
    int64_t       m_periodNs;
    int64_t       m_nextDisplayNs;
    bool          m_first = true;
};

} // namespace xr3dv
