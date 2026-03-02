# XR3DV Install Script
# Registers XR3DV as the active OpenXR runtime.
# Must be run as Administrator.

param(
    [string]$DllPath = ""
)

$ErrorActionPreference = "Stop"

# Locate the manifest
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($DllPath -eq "") {
    $jsonPath = Join-Path $scriptDir "xr3dv.json"
} else {
    $jsonPath = Join-Path (Split-Path -Parent $DllPath) "xr3dv.json"
}

if (-not (Test-Path $jsonPath)) {
    Write-Error "Cannot find xr3dv.json at: $jsonPath"
    exit 1
}

$absJson = Resolve-Path $jsonPath | Select-Object -ExpandProperty Path
Write-Host "Registering XR3DV runtime: $absJson"

$regKey = "HKLM:\SOFTWARE\Khronos\OpenXR\1"
if (-not (Test-Path $regKey)) {
    New-Item -Path $regKey -Force | Out-Null
}

# Backup existing ActiveRuntime
$existing = Get-ItemProperty -Path $regKey -Name "ActiveRuntime" -ErrorAction SilentlyContinue
if ($existing) {
    $backup = $existing.ActiveRuntime
    Write-Host "Previous ActiveRuntime: $backup"
    Set-ItemProperty -Path $regKey -Name "ActiveRuntime_XR3DV_Backup" -Value $backup
}

Set-ItemProperty -Path $regKey -Name "ActiveRuntime" -Value $absJson
Write-Host "XR3DV registered successfully as the active OpenXR runtime."

# Copy example config if xr3dv.ini doesn't exist yet
$iniDst = Join-Path (Split-Path -Parent $absJson) "xr3dv.ini"
$iniSrc = Join-Path $scriptDir "xr3dv.ini.example"
if (-not (Test-Path $iniDst) -and (Test-Path $iniSrc)) {
    Copy-Item $iniSrc $iniDst
    Write-Host "Default config written to: $iniDst"
}

Write-Host ""
Write-Host "Done! Launch any OpenXR application to use NVIDIA 3D Vision via XR3DV."
Write-Host "Edit $iniDst to adjust resolution, frame rate, separation and convergence."
