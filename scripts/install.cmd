@echo off
::  XR3DV install.cmd
::  Registers XR3DV as the active OpenXR runtime.
::  Usage:
::    install.cmd                  — HKCU (always) + HKLM (if admin)
::    install.cmd /user            — HKCU only
::    install.cmd /dir "C:\path"   — explicit install directory
setlocal EnableDelayedExpansion

:: ── Defaults ────────────────────────────────────────────────────────────────
set "INSTALL_DIR=%~dp0"
set "USER_ONLY=0"

:: ── Parse arguments ─────────────────────────────────────────────────────────
:parse
if /i "%~1"=="/dir"  ( set "INSTALL_DIR=%~2" & shift & shift & goto parse )
if /i "%~1"=="/user" ( set "USER_ONLY=1"     & shift        & goto parse )
if not "%~1"=="" ( echo Unknown option: %1 & goto :usage )

:: Strip trailing backslash
if "!INSTALL_DIR:~-1!"=="\" set "INSTALL_DIR=!INSTALL_DIR:~0,-1!"

set "DLL_PATH=!INSTALL_DIR!\xr3dv.dll"
set "JSON_PATH=!INSTALL_DIR!\xr3dv.json"

:: ── Validate files ──────────────────────────────────────────────────────────
if not exist "!DLL_PATH!" (
    echo [ERROR] xr3dv.dll not found in: !INSTALL_DIR!
    exit /b 1
)
if not exist "!JSON_PATH!" (
    echo [ERROR] xr3dv.json not found in: !INSTALL_DIR!
    exit /b 1
)

:: ── Patch library_path in JSON (PowerShell one-liner, no script needed) ────
echo Patching manifest: !JSON_PATH!
set "FWD_DLL=!DLL_PATH:\=/!"
powershell -NoProfile -Command ^
  "(Get-Content '!JSON_PATH!' -Raw) -replace '\"library_path\"\s*:\s*\"[^\"]*\"', '\"library_path\": \"!FWD_DLL!\"' | Set-Content '!JSON_PATH!' -Encoding UTF8"
if errorlevel 1 ( echo [WARN] JSON patch failed — library_path may be wrong. )
echo   library_path ^-^> !FWD_DLL!

:: ── HKCU registration (always, no admin needed) ─────────────────────────────
echo.
echo Registering HKCU...
call :WriteReg "HKCU\SOFTWARE\Khronos\OpenXR\1"              "!JSON_PATH!"
call :WriteReg "HKCU\SOFTWARE\WOW6432Node\Khronos\OpenXR\1"  "!JSON_PATH!"

:: ── HKLM registration (admin only) ─────────────────────────────────────────
if "%USER_ONLY%"=="1" goto :skip_hklm

:: Check for admin rights
net session >nul 2>&1
if errorlevel 1 (
    echo [INFO] Not running as Administrator — HKLM skipped.
    echo        Re-run as Administrator for system-wide registration.
    goto :skip_hklm
)
echo.
echo Registering HKLM (system-wide)...
call :WriteReg "HKLM\SOFTWARE\Khronos\OpenXR\1"              "!JSON_PATH!"
call :WriteReg "HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1"  "!JSON_PATH!"

:skip_hklm

:: ── Deploy default config if missing ────────────────────────────────────────
if not exist "!INSTALL_DIR!\xr3dv.ini" (
    if exist "!INSTALL_DIR!\xr3dv.ini.example" (
        copy /y "!INSTALL_DIR!\xr3dv.ini.example" "!INSTALL_DIR!\xr3dv.ini" >nul
        echo Default config written: !INSTALL_DIR!\xr3dv.ini
    )
)

:: ── Done ─────────────────────────────────────────────────────────────────────
echo.
echo ==================================================
echo  XR3DV registered as the active OpenXR runtime.
echo  Run xr3dv_diag.exe to verify the full stack.
echo ==================================================
endlocal
exit /b 0

:: ── Subroutine: write one registry value ────────────────────────────────────
:WriteReg
::  %1 = full key path (no leading backslash)  e.g. HKCU\SOFTWARE\Khronos\OpenXR\1
::  %2 = JSON absolute path
reg add "%~1" /v "ActiveRuntime" /t REG_SZ /d "%~2" /f >nul 2>&1
if errorlevel 1 (
    echo   [FAIL] %~1
) else (
    echo   [OK]   %~1
)
exit /b 0

:usage
echo Usage: install.cmd [/user] [/dir "path"]
exit /b 1
