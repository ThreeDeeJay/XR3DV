# XR3DV Install Script
# Registers XR3DV as the active OpenXR runtime.
# Writes the absolute DLL path into the manifest so the OpenXR loader can
# always find it regardless of working directory.
#
# Run as Administrator for HKLM (system-wide).
# Run as a normal user   for HKCU (per-user, takes priority over HKLM).

param(
    [string]$InstallDir  = "",   # Override: directory containing xr3dv.dll
    [switch]$UserOnly,           # Register in HKCU only (no admin needed)
    [switch]$Uninstall           # Remove registration instead of adding it
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2

# ---------------------------------------------------------------------------
# Resolve install directory
# ---------------------------------------------------------------------------
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if ($InstallDir -eq "") {
    $InstallDir = $scriptDir
}
$InstallDir = (Resolve-Path $InstallDir).Path

$dllPath  = Join-Path $InstallDir "xr3dv.dll"
$jsonPath = Join-Path $InstallDir "xr3dv.json"

if (-not (Test-Path $dllPath)) {
    Write-Error "xr3dv.dll not found in: $InstallDir`nBuild the project first or pass -InstallDir <path>"
    exit 1
}
if (-not (Test-Path $jsonPath)) {
    Write-Error "xr3dv.json not found in: $InstallDir"
    exit 1
}

# ---------------------------------------------------------------------------
# Patch the JSON so library_path is the absolute DLL path.
# The OpenXR loader spec allows relative paths (resolved from the JSON's own
# directory), but absolute paths are unambiguous and work in every loader.
# ---------------------------------------------------------------------------
function Update-JsonLibraryPath {
    param([string]$JsonFile, [string]$DllAbsPath)
    $raw = Get-Content $JsonFile -Raw
    $fwd = $DllAbsPath.Replace('\', '/')
    $updated = $raw -replace '"library_path"\s*:\s*"[^"]*"',
                              ('"library_path": "' + $fwd + '"')
    if ($updated -eq $raw) {
        Write-Warning "library_path not updated (pattern not matched)"
    } else {
        Set-Content -Path $JsonFile -Value $updated -Encoding UTF8
        Write-Host "  library_path set to: $fwd"
    }
}

$absJson = (Resolve-Path $jsonPath).Path
$absDll  = (Resolve-Path $dllPath).Path

# ---------------------------------------------------------------------------
# Uninstall path
# ---------------------------------------------------------------------------
if ($Uninstall) {
    foreach ($hive in @("HKLM", "HKCU")) {
        $key = "${hive}:\SOFTWARE\Khronos\OpenXR\1"
        $cur = (Get-ItemProperty -Path $key -Name "ActiveRuntime" -ErrorAction SilentlyContinue).ActiveRuntime
        if ($cur -like "*xr3dv*") {
            $backup = (Get-ItemProperty -Path $key -Name "ActiveRuntime_XR3DV_Backup" -ErrorAction SilentlyContinue).ActiveRuntime_XR3DV_Backup
            if ($backup) {
                Set-ItemProperty -Path $key -Name "ActiveRuntime" -Value $backup
                Remove-ItemProperty -Path $key -Name "ActiveRuntime_XR3DV_Backup" -ErrorAction SilentlyContinue
                Write-Host "[$hive] Restored previous runtime: $backup"
            } else {
                Remove-ItemProperty -Path $key -Name "ActiveRuntime" -ErrorAction SilentlyContinue
                Write-Host "[$hive] ActiveRuntime removed (no backup found)"
            }
        } else {
            Write-Host "[$hive] XR3DV was not the active runtime — skipped"
        }
    }
    Write-Host "Uninstall complete."
    exit 0
}

# ---------------------------------------------------------------------------
# Patch JSON with absolute DLL path
# ---------------------------------------------------------------------------
Write-Host "Patching manifest: $absJson"
Update-JsonLibraryPath -JsonFile $absJson -DllAbsPath $absDll

# ---------------------------------------------------------------------------
# Register in registry
# ---------------------------------------------------------------------------
function Register-Runtime {
    param([string]$Hive, [string]$JsonAbsPath)
    $key = "${Hive}:\SOFTWARE\Khronos\OpenXR\1"
    if (-not (Test-Path $key)) {
        New-Item -Path $key -Force | Out-Null
    }
    $cur = (Get-ItemProperty -Path $key -Name "ActiveRuntime" -ErrorAction SilentlyContinue).ActiveRuntime
    if ($cur -and ($cur -notlike "*xr3dv*")) {
        Write-Host "  [$Hive] Backing up existing runtime: $cur"
        Set-ItemProperty -Path $key -Name "ActiveRuntime_XR3DV_Backup" -Value $cur
    }
    Set-ItemProperty -Path $key -Name "ActiveRuntime" -Type String -Value $JsonAbsPath
    $readback = (Get-ItemProperty -Path $key -Name "ActiveRuntime").ActiveRuntime
    if ($readback -eq $JsonAbsPath) {
        Write-Host "  [$Hive] Registered OK: $JsonAbsPath"
    } else {
        Write-Warning "  [$Hive] Readback mismatch! Got: $readback"
    }
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

Write-Host "`nRegistering in HKCU (per-user, loader priority)..."
Register-Runtime -Hive "HKCU" -JsonAbsPath $absJson

if ($isAdmin -and -not $UserOnly) {
    Write-Host "Registering in HKLM (system-wide)..."
    Register-Runtime -Hive "HKLM" -JsonAbsPath $absJson
} elseif (-not $isAdmin) {
    Write-Host "  [HKLM] Skipped — run as Administrator for system-wide registration."
}

# ---------------------------------------------------------------------------
# Deploy default config if missing
# ---------------------------------------------------------------------------
$iniDst = Join-Path $InstallDir "xr3dv.ini"
$iniSrc = Join-Path $scriptDir  "xr3dv.ini.example"
if (-not (Test-Path $iniDst)) {
    if (Test-Path $iniSrc) {
        Copy-Item $iniSrc $iniDst
        Write-Host "`nDefault config written to: $iniDst"
    }
}

# ---------------------------------------------------------------------------
# Quick sanity check
# ---------------------------------------------------------------------------
Write-Host "`nValidating manifest JSON..."
try {
    $json = Get-Content $absJson -Raw | ConvertFrom-Json
    $libPath = $json.runtime.library_path
    Write-Host "  file_format_version : $($json.file_format_version)"
    Write-Host "  runtime.name        : $($json.runtime.name)"
    Write-Host "  runtime.api_version : $($json.runtime.api_version)"
    Write-Host "  library_path        : $libPath"
    if (Test-Path $libPath) {
        Write-Host "  DLL exists          : YES"
    } else {
        Write-Warning "  DLL exists          : NO — loader will fail!"
    }
} catch {
    Write-Warning "JSON parse error: $_"
}

Write-Host "`n================================================"
Write-Host " XR3DV registered as the active OpenXR runtime."
Write-Host " Run xr3dv_diag.exe to verify the full stack."
Write-Host "================================================`n"
