@echo off
::  XR3DV uninstall.cmd
::  Removes XR3DV ActiveRuntime entries and restores any backed-up runtime.
setlocal EnableDelayedExpansion

echo Uninstalling XR3DV OpenXR runtime registration...
echo.

call :RemoveReg "HKCU\SOFTWARE\Khronos\OpenXR\1"
call :RemoveReg "HKCU\SOFTWARE\WOW6432Node\Khronos\OpenXR\1"

net session >nul 2>&1
if errorlevel 1 (
    echo [INFO] Not Administrator — HKLM entries not removed.
    echo        Re-run as Administrator to remove system-wide keys.
    goto :done
)
call :RemoveReg "HKLM\SOFTWARE\Khronos\OpenXR\1"
call :RemoveReg "HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1"

:done
echo.
echo Uninstall complete.
endlocal
exit /b 0

:RemoveReg
:: Read current value; skip if not xr3dv
for /f "tokens=3*" %%A in ('reg query "%~1" /v "ActiveRuntime" 2^>nul') do set "CUR=%%A %%B"
if "!CUR!"=="" ( echo   [SKIP] %~1 not set & exit /b 0 )
echo !CUR! | findstr /i "xr3dv" >nul
if errorlevel 1 ( echo   [SKIP] %~1 not xr3dv ^(!CUR!^) & exit /b 0 )

:: Try to restore backup
for /f "tokens=3*" %%A in ('reg query "%~1" /v "ActiveRuntime_XR3DV_Backup" 2^>nul') do set "BK=%%A %%B"
if not "!BK!"=="" (
    reg add "%~1" /v "ActiveRuntime" /t REG_SZ /d "!BK!" /f >nul
    reg delete "%~1" /v "ActiveRuntime_XR3DV_Backup" /f >nul 2>&1
    echo   [OK]   %~1 restored to: !BK!
) else (
    reg delete "%~1" /v "ActiveRuntime" /f >nul 2>&1
    echo   [OK]   %~1 ActiveRuntime removed ^(no backup^)
)
exit /b 0
