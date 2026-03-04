# XR3DV Uninstall Script
# Removes XR3DV from the OpenXR runtime registry and restores the previous runtime.
# Run as Administrator to also clear the HKLM entry.

$ErrorActionPreference = 'Stop'
# NOTE: StrictMode intentionally omitted - accessing absent registry properties
# throws under StrictMode even with -ErrorAction SilentlyContinue.

function Get-RegValue {
    param([string]$Path, [string]$Name)
    try {
        $obj = Get-ItemProperty -Path $Path -Name $Name -ErrorAction Stop
        return $obj.$Name
    } catch {
        return $null
    }
}

foreach ($hive in @('HKLM', 'HKCU')) {
    $key = $hive + ':\SOFTWARE\Khronos\OpenXR\1'
    if (-not (Test-Path $key)) {
        Write-Host ('[' + $hive + '] Key does not exist - skipped.')
        continue
    }

    $cur = Get-RegValue -Path $key -Name 'ActiveRuntime'
    if ($null -eq $cur -or $cur -notlike '*xr3dv*') {
        Write-Host ('[' + $hive + '] XR3DV is not the active runtime - skipped.')
        continue
    }

    $bk = Get-RegValue -Path $key -Name 'ActiveRuntime_XR3DV_Backup'
    if ($null -ne $bk) {
        Set-ItemProperty -Path $key -Name 'ActiveRuntime' -Value $bk
        Remove-ItemProperty -Path $key -Name 'ActiveRuntime_XR3DV_Backup' -ErrorAction SilentlyContinue
        Write-Host ('[' + $hive + '] Restored previous runtime: ' + $bk)
    } else {
        Remove-ItemProperty -Path $key -Name 'ActiveRuntime' -ErrorAction SilentlyContinue
        Write-Host ('[' + $hive + '] Removed ActiveRuntime (no backup found).')
    }
}

Write-Host 'XR3DV unregistered successfully.'
