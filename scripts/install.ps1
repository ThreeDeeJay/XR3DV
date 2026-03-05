# XR3DV Install Script
# Registers XR3DV as the active OpenXR runtime.
# Patches xr3dv.json with the absolute DLL path so the OpenXR loader
# always finds the DLL regardless of working directory.
#
# Usage:
#   .\install.ps1                   # auto-detect dir, HKCU + HKLM (if admin)
#   .\install.ps1 -InstallDir <p>   # explicit install directory
#   .\install.ps1 -UserOnly         # HKCU only, no admin needed
#   .\install.ps1 -NoWow64          # skip WOW6432Node (32-bit) registration
#   .\install.ps1 -Uninstall        # remove registration

param(
    [string]$InstallDir = '',
    [switch]$UserOnly,
    [switch]$NoWow64,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Safe registry read: returns $null instead of throwing when key/value absent
# ---------------------------------------------------------------------------
function Get-RegValue {
    param([string]$Path, [string]$Name)
    try {
        $obj = Get-ItemProperty -Path $Path -Name $Name -ErrorAction Stop
        return $obj.$Name
    } catch {
        return $null
    }
}

# ---------------------------------------------------------------------------
# Resolve paths
# ---------------------------------------------------------------------------
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($InstallDir -eq '') { $InstallDir = $scriptDir }
$InstallDir = (Resolve-Path $InstallDir).Path

$dllPath  = Join-Path $InstallDir 'xr3dv.dll'
$jsonPath = Join-Path $InstallDir 'xr3dv.json'

if (-not (Test-Path $dllPath)) {
    Write-Error ('xr3dv.dll not found in: ' + $InstallDir)
    exit 1
}
if (-not (Test-Path $jsonPath)) {
    Write-Error ('xr3dv.json not found in: ' + $InstallDir)
    exit 1
}

$absJson = (Resolve-Path $jsonPath).Path
$absDll  = (Resolve-Path $dllPath).Path

# ---------------------------------------------------------------------------
# Patch library_path in JSON to absolute DLL path
# ---------------------------------------------------------------------------
function Update-JsonLibraryPath {
    param([string]$JsonFile, [string]$DllAbsPath)
    $raw  = Get-Content $JsonFile -Raw
    $fwd  = $DllAbsPath.Replace('\', '/')
    $updated = $raw -replace '"library_path"\s*:\s*"[^"]*"',
                              ('"library_path": "' + $fwd + '"')
    if ($updated -eq $raw) {
        Write-Warning 'library_path pattern not matched in JSON.'
    } else {
        Set-Content -Path $JsonFile -Value $updated -Encoding UTF8
        Write-Host ('  library_path -> ' + $fwd)
    }
}

# ---------------------------------------------------------------------------
# Register runtime in one registry hive
# ---------------------------------------------------------------------------
function Register-Runtime {
    param([string]$Hive, [string]$JsonAbsPath, [bool]$Wow64 = $false)
    $subkey = if ($Wow64) { 'SOFTWARE\WOW6432Node\Khronos\OpenXR\1' } else { 'SOFTWARE\Khronos\OpenXR\1' }
    $key    = $Hive + ':\' + $subkey
    $label  = if ($Wow64) { $Hive + ' WOW64' } else { $Hive }

    if (-not (Test-Path $key)) {
        New-Item -Path $key -Force | Out-Null
        Write-Host ('  [' + $label + '] Created registry key.')
    }

    $cur = Get-RegValue -Path $key -Name 'ActiveRuntime'
    if (($null -ne $cur) -and ($cur -notlike '*xr3dv*')) {
        Write-Host ('  [' + $label + '] Backing up existing runtime: ' + $cur)
        Set-ItemProperty -Path $key -Name 'ActiveRuntime_XR3DV_Backup' -Value $cur
    }

    Set-ItemProperty -Path $key -Name 'ActiveRuntime' -Type String -Value $JsonAbsPath

    $rb = Get-RegValue -Path $key -Name 'ActiveRuntime'
    if ($rb -eq $JsonAbsPath) {
        Write-Host ('  [' + $label + '] OK: ' + $JsonAbsPath)
    } else {
        Write-Warning ('  [' + $label + '] Readback mismatch. Got: ' + $rb)
    }
}

# ---------------------------------------------------------------------------
# Uninstall path
# ---------------------------------------------------------------------------
if ($Uninstall) {
    $paths = @(
        @{ Hive = 'HKCU'; Sub = 'SOFTWARE\Khronos\OpenXR\1' },
        @{ Hive = 'HKLM'; Sub = 'SOFTWARE\Khronos\OpenXR\1' },
        @{ Hive = 'HKCU'; Sub = 'SOFTWARE\WOW6432Node\Khronos\OpenXR\1' },
        @{ Hive = 'HKLM'; Sub = 'SOFTWARE\WOW6432Node\Khronos\OpenXR\1' }
    )
    foreach ($p in $paths) {
        $key = $p.Hive + ':\' + $p.Sub
        if (-not (Test-Path $key)) { continue }
        $cur = Get-RegValue -Path $key -Name 'ActiveRuntime'
        if ($null -eq $cur -or $cur -notlike '*xr3dv*') {
            Write-Host ('[' + $p.Hive + '\' + $p.Sub + '] XR3DV was not active - skipped.')
            continue
        }
        $bk = Get-RegValue -Path $key -Name 'ActiveRuntime_XR3DV_Backup'
        if ($null -ne $bk) {
            Set-ItemProperty -Path $key -Name 'ActiveRuntime' -Value $bk
            Remove-ItemProperty -Path $key -Name 'ActiveRuntime_XR3DV_Backup' -ErrorAction SilentlyContinue
            Write-Host ('[' + $p.Hive + '\' + $p.Sub + '] Restored: ' + $bk)
        } else {
            Remove-ItemProperty -Path $key -Name 'ActiveRuntime' -ErrorAction SilentlyContinue
            Write-Host ('[' + $p.Hive + '\' + $p.Sub + '] Removed (no backup found).')
        }
    }
    Write-Host 'Uninstall complete.'
    exit 0
}

# ---------------------------------------------------------------------------
# Patch + Register
# ---------------------------------------------------------------------------
Write-Host ('Patching manifest: ' + $absJson)
Update-JsonLibraryPath -JsonFile $absJson -DllAbsPath $absDll

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

Write-Host ''
Write-Host 'Registering in HKCU (per-user, loader priority)...'
Register-Runtime -Hive 'HKCU' -JsonAbsPath $absJson -Wow64 $false

if (-not $NoWow64) {
    Write-Host 'Registering in HKCU WOW6432Node (32-bit app support)...'
    Register-Runtime -Hive 'HKCU' -JsonAbsPath $absJson -Wow64 $true
}

if ($isAdmin -and -not $UserOnly) {
    Write-Host 'Registering in HKLM (system-wide)...'
    Register-Runtime -Hive 'HKLM' -JsonAbsPath $absJson -Wow64 $false
    if (-not $NoWow64) {
        Write-Host 'Registering in HKLM WOW6432Node (32-bit app support)...'
        Register-Runtime -Hive 'HKLM' -JsonAbsPath $absJson -Wow64 $true
    }
} elseif (-not $isAdmin) {
    Write-Host '  [HKLM] Skipped - run as Administrator for system-wide registration.'
}

# ---------------------------------------------------------------------------
# Deploy default config if missing
# ---------------------------------------------------------------------------
$iniDst = Join-Path $InstallDir 'xr3dv.ini'
$iniSrc = Join-Path $scriptDir  'xr3dv.ini.example'
if ((-not (Test-Path $iniDst)) -and (Test-Path $iniSrc)) {
    Copy-Item $iniSrc $iniDst
    Write-Host ('Default config written to: ' + $iniDst)
}

# ---------------------------------------------------------------------------
# Validate the JSON
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host 'Validating manifest...'
try {
    $json    = Get-Content $absJson -Raw | ConvertFrom-Json
    $libPath = $json.runtime.library_path
    Write-Host ('  file_format_version : ' + $json.file_format_version)
    Write-Host ('  api_version         : ' + $json.runtime.api_version)
    Write-Host ('  library_path        : ' + $libPath)
    if (Test-Path $libPath) {
        Write-Host '  DLL exists          : YES'
    } else {
        Write-Warning ('  DLL NOT FOUND at: ' + $libPath + ' - loader will fail!')
    }
} catch {
    Write-Warning ('JSON parse error: ' + $_)
}

Write-Host ''
Write-Host '=================================================='
Write-Host ' XR3DV registered as the active OpenXR runtime.'
Write-Host ' 64-bit and 32-bit (WOW64) registry keys written.'
Write-Host ' Run xr3dv_diag.exe to verify the full stack.'
Write-Host '  NOTE: 32-bit apps need a separate xr3dv_x86.dll.'
Write-Host '  Build with: cmake -A Win32 -B build32'
Write-Host '=================================================='
