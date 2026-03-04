# XR3DV Install Script
# Registers XR3DV as the active OpenXR runtime.
# Patches the JSON manifest with the absolute DLL path so the loader always
# finds the DLL regardless of working directory.
#
# Usage:
#   .\install.ps1                  # auto-detect dir, write HKCU + HKLM (if admin)
#   .\install.ps1 -InstallDir <p>  # explicit install directory
#   .\install.ps1 -UserOnly        # HKCU only, no admin needed
#   .\install.ps1 -Uninstall       # remove registration, restore previous

param(
    [string]$InstallDir = "",
    [switch]$UserOnly,
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2

# ---------------------------------------------------------------------------
# Resolve install directory
# ---------------------------------------------------------------------------
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($InstallDir -eq "") { $InstallDir = $scriptDir }
$InstallDir = (Resolve-Path $InstallDir).Path

$dllPath  = Join-Path $InstallDir "xr3dv.dll"
$jsonPath = Join-Path $InstallDir "xr3dv.json"

if (-not (Test-Path $dllPath)) {
    Write-Error ("xr3dv.dll not found in: " + $InstallDir + "`nBuild first or pass -InstallDir <path>")
    exit 1
}
if (-not (Test-Path $jsonPath)) {
    Write-Error ("xr3dv.json not found in: " + $InstallDir)
    exit 1
}

$absJson = (Resolve-Path $jsonPath).Path
$absDll  = (Resolve-Path $dllPath).Path

# ---------------------------------------------------------------------------
# Patch library_path in JSON to the absolute DLL path
# ---------------------------------------------------------------------------
function Update-JsonLibraryPath {
    param([string]$JsonFile, [string]$DllAbsPath)
    $raw = Get-Content $JsonFile -Raw
    $fwd = $DllAbsPath.Replace('\', '/')
    $updated = $raw -replace '"library_path"\s*:\s*"[^"]*"',
                              ('"library_path": "' + $fwd + '"')
    if ($updated -eq $raw) {
        Write-Warning "library_path pattern not matched in JSON."
    } else {
        Set-Content -Path $JsonFile -Value $updated -Encoding UTF8
        Write-Host ("  library_path -> " + $fwd)
    }
}

# ---------------------------------------------------------------------------
# Register a runtime in a given registry hive
# ---------------------------------------------------------------------------
function Register-Runtime {
    param([string]$Hive, [string]$JsonAbsPath)
    $key = $Hive + ":\SOFTWARE\Khronos\OpenXR\1"
    if (-not (Test-Path $key)) {
        New-Item -Path $key -Force | Out-Null
    }
    $cur = (Get-ItemProperty -Path $key -Name "ActiveRuntime" -ErrorAction SilentlyContinue).ActiveRuntime
    if ($cur -and ($cur -notlike "*xr3dv*")) {
        Write-Host ("  [" + $Hive + "] Backing up: " + $cur)
        Set-ItemProperty -Path $key -Name "ActiveRuntime_XR3DV_Backup" -Value $cur
    }
    Set-ItemProperty -Path $key -Name "ActiveRuntime" -Type String -Value $JsonAbsPath
    $rb = (Get-ItemProperty -Path $key -Name "ActiveRuntime").ActiveRuntime
    if ($rb -eq $JsonAbsPath) {
        Write-Host ("  [" + $Hive + "] OK: " + $JsonAbsPath)
    } else {
        Write-Warning ("  [" + $Hive + "] Readback mismatch: " + $rb)
    }
}

# ---------------------------------------------------------------------------
# Uninstall path
# ---------------------------------------------------------------------------
if ($Uninstall) {
    foreach ($hive in @("HKLM", "HKCU")) {
        $key = $hive + ":\SOFTWARE\Khronos\OpenXR\1"
        $cur = (Get-ItemProperty -Path $key -Name "ActiveRuntime" -ErrorAction SilentlyContinue).ActiveRuntime
        if ($cur -like "*xr3dv*") {
            $bk = (Get-ItemProperty -Path $key -Name "ActiveRuntime_XR3DV_Backup" -ErrorAction SilentlyContinue).ActiveRuntime_XR3DV_Backup
            if ($bk) {
                Set-ItemProperty -Path $key -Name "ActiveRuntime" -Value $bk
                Remove-ItemProperty -Path $key -Name "ActiveRuntime_XR3DV_Backup" -ErrorAction SilentlyContinue
                Write-Host ("[" + $hive + "] Restored: " + $bk)
            } else {
                Remove-ItemProperty -Path $key -Name "ActiveRuntime" -ErrorAction SilentlyContinue
                Write-Host ("[" + $hive + "] Removed (no backup).")
            }
        } else {
            Write-Host ("[" + $hive + "] XR3DV was not active - skipped.")
        }
    }
    Write-Host "Uninstall complete."
    exit 0
}

# ---------------------------------------------------------------------------
# Patch + Register
# ---------------------------------------------------------------------------
Write-Host ("Patching manifest: " + $absJson)
Update-JsonLibraryPath -JsonFile $absJson -DllAbsPath $absDll

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

Write-Host "Registering in HKCU (per-user, loader priority)..."
Register-Runtime -Hive "HKCU" -JsonAbsPath $absJson

if ($isAdmin -and -not $UserOnly) {
    Write-Host "Registering in HKLM (system-wide)..."
    Register-Runtime -Hive "HKLM" -JsonAbsPath $absJson
} elseif (-not $isAdmin) {
    Write-Host "  [HKLM] Skipped - run as Administrator for system-wide registration."
}

# ---------------------------------------------------------------------------
# Deploy default config if missing
# ---------------------------------------------------------------------------
$iniDst = Join-Path $InstallDir "xr3dv.ini"
$iniSrc = Join-Path $scriptDir  "xr3dv.ini.example"
if (-not (Test-Path $iniDst) -and (Test-Path $iniSrc)) {
    Copy-Item $iniSrc $iniDst
    Write-Host ("Default config written to: " + $iniDst)
}

# ---------------------------------------------------------------------------
# Validate the JSON
# ---------------------------------------------------------------------------
Write-Host "Validating manifest..."
try {
    $json    = Get-Content $absJson -Raw | ConvertFrom-Json
    $libPath = $json.runtime.library_path
    Write-Host ("  file_format_version : " + $json.file_format_version)
    Write-Host ("  api_version         : " + $json.runtime.api_version)
    Write-Host ("  library_path        : " + $libPath)
    if (Test-Path $libPath) {
        Write-Host "  DLL exists          : YES"
    } else {
        Write-Warning ("  DLL NOT FOUND at: " + $libPath + " - loader will fail!")
    }
} catch {
    Write-Warning ("JSON parse error: " + $_)
}

Write-Host "================================================"
Write-Host "XR3DV registered as the active OpenXR runtime."
Write-Host "Run xr3dv_diag.exe to verify the full stack."
Write-Host "================================================"
