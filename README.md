# XR3DV — OpenXR Runtime for NVIDIA 3D Vision

**XR3DV** is a minimal Windows OpenXR runtime that bridges the OpenXR API to
NVIDIA 3D Vision hardware via NVAPI and Direct3D 9.  It purposefully discards
all VR-specific components — head-tracking, motion controllers, haptics — and
exposes only stereoscopic rendering, letting classic 3D Vision glasses display
OpenXR content with full left/right eye separation.

```
OpenXR Application
       │  (OpenXR 1.0 API)
       ▼
   XR3DV.dll  ◄──── xr3dv.json  (runtime manifest)
       │
       │  D3D11 swapchain surfaces exposed to app
       │  Blit → D3D9 stereo surface via NVAPI
       ▼
NVIDIA 3D Vision Driver
       │
       ▼
  3D Vision IR emitter + glasses
```

## Features

| Feature | Status |
|---|---|
| OpenXR 1.0 core runtime | ✅ |
| Stereo left/right view rendering | ✅ |
| NVIDIA 3D Vision via NVAPI | ✅ |
| Direct3D 9 stereo presentation | ✅ |
| D3D11 swapchain (for OpenXR apps) | ✅ |
| Configurable resolution | ✅ |
| Configurable frame rate | ✅ |
| Configurable separation | ✅ |
| Configurable convergence | ✅ |
| Head tracking | ❌ (identity pose) |
| Motion controllers | ❌ (stubbed) |
| Haptics | ❌ (no-op) |
| Hand tracking | ❌ (not exposed) |

## Requirements

- Windows 10/11 64-bit
- NVIDIA GPU with 3D Vision support (GeForce GTX 400 series or newer)
- NVIDIA driver with 3D Vision enabled (≤ driver 425.31 recommended; newer
  drivers dropped 3D Vision support on some SKUs)
- NVAPI SDK (headers included in `include/nvapi/`)
- DirectX SDK (June 2010) or Windows SDK with D3D9 + D3D11 headers
- Visual Studio 2022 or later (MSVC) with CMake ≥ 3.21
- OpenXR SDK headers (fetched automatically by CMake via FetchContent)

## Building

```bat
git clone https://github.com/your-org/XR3DV.git
cd XR3DV

:: Copy NVAPI SDK headers into include\nvapi\
:: (download from https://developer.nvidia.com/nvapi)
xcopy /E /I path\to\nvapi\amd64 include\nvapi\

cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The output is:
- `build\Release\xr3dv.dll`  — the OpenXR runtime DLL
- `build\Release\xr3dv.json` — the runtime manifest (auto-generated)

## Installation

### Register the runtime

Run the included PowerShell script **as Administrator**:

```powershell
.\scripts\install.ps1
```

Or register manually:

```bat
reg add "HKLM\SOFTWARE\Khronos\OpenXR\1" /v ActiveRuntime /t REG_SZ /d "C:\path\to\xr3dv.json" /f
```

### Configuration

Edit `xr3dv.ini` (created next to the DLL on first run, or copy from
`config\xr3dv.ini.example`):

```ini
[Display]
; Render resolution per eye
Width=1920
Height=1080
; Target frame rate (Hz)
FrameRate=120

[Stereo]
; Eye separation as percentage (0.0–100.0)
Separation=50.0
; Convergence distance (arbitrary units, 0.0–25.0)
Convergence=5.0
; Interpupillary distance in metres
IPD=0.064

[Debug]
; 0=Off 1=Errors 2=Verbose
LogLevel=1
LogFile=xr3dv.log
```

## Architecture

### OpenXR layer implemented

XR3DV implements the **OpenXR Loader Protocol** (runtime DLL):

| XR object | Implementation |
|---|---|
| `XrInstance` | Validates extensions; returns driver info |
| `XrSession` | Creates D3D11 device + D3D9 stereo device |
| `XrSwapchain` | D3D11 texture arrays exposed to app |
| `XrSpace` | Identity transforms — no tracking |
| `XrActionSet / XrAction` | Stub; inputs always zero |
| `XrFrameState` | Timing driven by QPC + target frame rate |

### NVAPI 3D Vision pipeline

1. A hidden D3D9 window is created at startup.
2. `NvAPI_Stereo_Enable()` enables stereo mode.
3. `NvAPI_Stereo_CreateHandleFromIUnknown()` wraps the D3D9 device.
4. Per frame:
   - `NvAPI_Stereo_SetActiveEye(LEFT)` → blit left swapchain image.
   - `NvAPI_Stereo_SetActiveEye(RIGHT)` → blit right swapchain image.
   - `IDirect3DDevice9::Present()` — driver sends stereo frame to glasses.
5. `NvAPI_Stereo_SetSeparation()` / `NvAPI_Stereo_SetConvergence()` apply
   depth parameters from config (can be hot-reloaded at runtime).

### Hot-reload

Changing `Separation` or `Convergence` in `xr3dv.ini` while the app is
running takes effect on the next frame (file is polled every 500 ms).

## License

XR3DV is released under the **GNU General Public License v3.0**.  
See [LICENSE](LICENSE) for full text.

NVAPI headers are Copyright NVIDIA Corporation and are subject to their
own license terms (see `include/nvapi/License.txt`).

OpenXR headers are Copyright The Khronos Group Inc. under the Apache 2.0
license.
