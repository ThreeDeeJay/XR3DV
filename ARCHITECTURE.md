# XR3DV Architecture

## Overview

XR3DV is a **minimal OpenXR 1.0 runtime DLL** that strips out all VR-centric
components — no head tracking, no controllers, no IMU — and routes the two
stereo eye images produced by any OpenXR application to NVIDIA 3D Vision
hardware via NVAPI and Direct3D 9.

```
┌────────────────────────────────────────────────────────────┐
│                   OpenXR Application                       │
│  (any engine: Unity/Unreal/OpenVR-compat/hand-rolled)      │
└────────────────┬───────────────────────────────────────────┘
                 │  OpenXR 1.0 API (D3D11 binding)
                 ▼
┌────────────────────────────────────────────────────────────┐
│                    XR3DV Runtime DLL                       │
│                                                            │
│  ┌──────────────────────────────────┐                      │
│  │      xrCreateInstance            │  main.cpp            │
│  │      xrCreateSession             │  runtime.cpp         │
│  │      xrNegotiateLoader...        │  (loader protocol)   │
│  └──────────┬───────────────────────┘                      │
│             │                                              │
│  ┌──────────▼───────────────────────┐                      │
│  │   Session (session.cpp)          │                      │
│  │   - Manages D3D11 device ref     │                      │
│  │   - Frame timing (QPC-based)     │                      │
│  │   - Swapchain registry           │                      │
│  │   - Config hot-reload thread     │                      │
│  └──────────┬───────────────────────┘                      │
│             │ left/right SRVs per frame                    │
│  ┌──────────▼───────────────────────┐                      │
│  │  NvapiStereoPresenter            │  nvapi_stereo.cpp    │
│  │  (nvapi_stereo.cpp)              │                      │
│  │  1. Creates hidden D3D9Ex window │                      │
│  │  2. NvAPI_Stereo_Enable()        │                      │
│  │  3. Per-frame:                   │                      │
│  │     a. D3D11→CPU→D3D9 blit (L)  │                      │
│  │     b. SetActiveEye(LEFT)        │                      │
│  │     c. StretchRect to back buf   │                      │
│  │     d. Same for RIGHT            │                      │
│  │     e. PresentEx()               │                      │
│  └──────────────────────────────────┘                      │
│                                                            │
│  ┌──────────────────────────────────┐                      │
│  │  Stub subsystems (no-op)         │                      │
│  │  - space.cpp   — identity poses  │                      │
│  │  - actions.cpp — zero inputs     │                      │
│  └──────────────────────────────────┘                      │
└────────────────┬───────────────────────────────────────────┘
                 │  D3D9 stereo Present
                 ▼
┌────────────────────────────────────────────────────────────┐
│          NVIDIA Display Driver (3D Vision)                  │
│  NvAPI_Stereo_SetSeparation / NvAPI_Stereo_SetConvergence  │
│  → 3D Vision IR Emitter → Active shutter glasses           │
└────────────────────────────────────────────────────────────┘
```

---

## File Map

| File | Responsibility |
|---|---|
| `src/main.cpp` | OpenXR loader negotiation, instance lifecycle, `xrGetInstanceProcAddr` dispatch table |
| `src/runtime.cpp` | Global singleton `Runtime`, config bootstrap |
| `src/session.cpp` | `XrSession` lifecycle, `WaitFrame/BeginFrame/EndFrame`, config poll thread |
| `src/swapchain.cpp` | D3D11 texture pool implementing OpenXR swapchain acquire/wait/release |
| `src/nvapi_stereo.cpp` | D3D9Ex device creation, NVAPI stereo handle, CPU-blit, `PresentEx` |
| `src/d3d9_presenter.cpp` | Optional GPU-shared-handle fast path (no CPU readback) |
| `src/space.cpp` | Identity-transform XrSpace stubs |
| `src/actions.cpp` | Zero-input action stubs |
| `src/timing.cpp` | QPC-based frame timer |
| `src/config.cpp` | INI parser, hot-reload |
| `src/logging.cpp` | Thread-safe logger |

---

## Frame Loop Detail

```
Application            XR3DV Runtime              NVAPI / D3D9
    │                      │                          │
    │  xrWaitFrame()       │                          │
    │─────────────────────►│  QPC sleep to next vsync │
    │◄─────────────────────│  predictedDisplayTime     │
    │                      │                          │
    │  xrBeginFrame()      │                          │
    │─────────────────────►│  m_frameBegun = true     │
    │                      │                          │
    │  [Render left eye]   │                          │
    │  [Render right eye]  │                          │
    │                      │                          │
    │  xrEndFrame()        │                          │
    │   layers[0] = proj   │                          │
    │     view[0]=left SC  │                          │
    │     view[1]=right SC │                          │
    │─────────────────────►│                          │
    │                      │  BlitD3D11→D3D9 (left)  │
    │                      │─────────────────────────►│
    │                      │  SetActiveEye(LEFT)      │
    │                      │─────────────────────────►│
    │                      │  StretchRect(left)       │
    │                      │─────────────────────────►│
    │                      │  BlitD3D11→D3D9 (right)  │
    │                      │─────────────────────────►│
    │                      │  SetActiveEye(RIGHT)     │
    │                      │─────────────────────────►│
    │                      │  StretchRect(right)      │
    │                      │─────────────────────────►│
    │                      │  PresentEx()             │
    │                      │─────────────────────────►│
    │                      │            [3D Vision frame sent to glasses]
```

---

## D3D11 → D3D9 Transfer

Direct API interop between D3D11 and D3D9 is limited.  XR3DV uses two paths:

### Path A — CPU Readback (default, always works)

1. Copy D3D11 render target → D3D11 STAGING texture (`CopyResource`).
2. `Map(D3D11_MAP_READ)` → iterate rows → `memcpy` into D3D9 SYSMEM surface.
3. `UpdateSurface(SYSMEM → DEFAULT)` → D3D9 StretchRect.

Latency: 1–2 ms extra per frame on a mid-range GPU.  Acceptable for ≤120 Hz.

### Path B — Shared Surface Handle (fast path, optional)

If `Swapchain::Init` creates textures with `D3D11_RESOURCE_MISC_SHARED`:

1. `IDXGIResource::GetSharedHandle()` → HANDLE.
2. D3D9 `CreateTexture(..., pSharedHandle)` opens the same surface.
3. StretchRect GPU-to-GPU — zero CPU copy.

Enable by setting `#define XR3DV_USE_SHARED_HANDLES 1` in `swapchain.cpp`.
Requires the application's DXGI adapter to be the same physical GPU.

---

## NVAPI Stereo Parameters

| Parameter | NVAPI Function | Range | Notes |
|---|---|---|---|
| Separation | `NvAPI_Stereo_SetSeparation` | 0–100 % | Stereo depth strength |
| Convergence | `NvAPI_Stereo_SetConvergence` | 0–25 | Zero-parallax distance |
| Active eye | `NvAPI_Stereo_SetActiveEye` | LEFT/RIGHT | Per-blit selection |

Both are applied every frame so hot-reload takes effect immediately.

---

## Tracking / Controller Philosophy

XR3DV deliberately returns **identity poses with full validity flags** for all
spaces.  This means:

- The application believes it has a valid head pose (at the origin, facing –Z).
- No drift, no jitter, no calibration needed.
- Applications that require *any* head rotation will render a static stereo
  scene — exactly what a fixed 3D Vision display expects.

Controllers report `isActive = XR_FALSE` so well-behaved applications display
"no controller" fallback UI or just skip controller input handling.

---

## Known Limitations

1. **3D Vision driver version**: NVIDIA dropped 3D Vision support in drivers
   after 425.31 for newer GPUs.  XR3DV works best with GeForce GTX 900/1000
   series + driver 425.31 or a Quadro with continued stereo support.

2. **Monitor requirement**: 3D Vision requires a 120 Hz display or a 3D Vision
   projector.  Running on a 60 Hz display will yield left-eye-only output.

3. **No multiview**: OpenXR multiview (`XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`
   with array swapchains) is supported; however the current blit code assumes
   separate single-layer swapchains for each eye.  Applications that submit
   one array swapchain for both eyes need the array slice extraction path (not
   yet implemented — PRs welcome).

4. **CPU blit latency**: The default CPU-readback path adds ~1–2 ms of extra
   latency.  Enable Path B (shared handles) for GPU-native transfer.
