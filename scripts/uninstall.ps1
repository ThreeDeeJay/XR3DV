# XR3DV Uninstall Script
# Removes XR3DV from the OpenXR runtime registry and restores the previous runtime.
# Run as Administrator.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$regKey = 'HKLM:\SOFTWARE\Khronos\OpenXR\1'

$current = (Get-ItemProperty -Path $regKey -Name 'ActiveRuntime' -ErrorAction SilentlyContinue).ActiveRuntime
if ($current -notlike '*xr3dv*') {
    Write-Host ('XR3DV is not currently the active runtime (' + $current + ').')
    exit 0
}

$backup = (Get-ItemProperty -Path $regKey -Name 'ActiveRuntime_XR3DV_Backup' -ErrorAction SilentlyContinue).ActiveRuntime_XR3DV_Backup
if ($backup) {
    Write-Host ('Restoring previous runtime: ' + $backup)
    Set-ItemProperty -Path $regKey -Name 'ActiveRuntime' -Value $backup
    Remove-ItemProperty -Path $regKey -Name 'ActiveRuntime_XR3DV_Backup' -ErrorAction SilentlyContinue
} else {
    Write-Host 'No backup found - removing ActiveRuntime key entirely.'
    Remove-ItemProperty -Path $regKey -Name 'ActiveRuntime' -ErrorAction SilentlyContinue
}

# Also clear HKCU if set
$hkcuKey = 'HKCU:\SOFTWARE\Khronos\OpenXR\1'
$hkcuVal = (Get-ItemProperty -Path $hkcuKey -Name 'ActiveRuntime' -ErrorAction SilentlyContinue).ActiveRuntime
if ($hkcuVal -like '*xr3dv*') {
    $hkcuBk = (Get-ItemProperty -Path $hkcuKey -Name 'ActiveRuntime_XR3DV_Backup' -ErrorAction SilentlyContinue).ActiveRuntime_XR3DV_Backup
    if ($hkcuBk) {
        Set-ItemProperty -Path $hkcuKey -Name 'ActiveRuntime' -Value $hkcuBk
        Remove-ItemProperty -Path $hkcuKey -Name 'ActiveRuntime_XR3DV_Backup' -ErrorAction SilentlyContinue
        Write-Host ('[HKCU] Restored: ' + $hkcuBk)
    } else {
        Remove-ItemProperty -Path $hkcuKey -Name 'ActiveRuntime' -ErrorAction SilentlyContinue
        Write-Host '[HKCU] Removed.'
    }
}

Write-Host 'XR3DV unregistered successfully.'
