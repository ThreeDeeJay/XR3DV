cmake_minimum_required(VERSION 3.21)
project(XR3DV VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "XR3DV is a Windows-only project.")
endif()

# ---------------------------------------------------------------------------
# Fetch OpenXR-SDK headers (loader interface + openxr.h)
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    OpenXR-SDK
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.0.34
    GIT_SHALLOW    TRUE
)
# We only need the OpenXR headers and loader interface, not to build the loader.
# The official openxr_loader.dll should be installed separately via the
# Khronos OpenXR SDK installer (https://github.com/KhronosGroup/OpenXR-SDK-Source/releases).
set(BUILD_LOADER  OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(OpenXR-SDK)

# ---------------------------------------------------------------------------
# NVAPI (user must supply headers in include/nvapi)
# ---------------------------------------------------------------------------
set(NVAPI_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include/nvapi" CACHE PATH
    "Path to NVAPI SDK include directory")

if(NOT EXISTS "${NVAPI_INCLUDE_DIR}/nvapi.h")
    message(WARNING
        "NVAPI headers not found at ${NVAPI_INCLUDE_DIR}. "
        "Download the NVAPI SDK from https://developer.nvidia.com/nvapi "
        "and copy the headers to include/nvapi/")
endif()

# NVAPI lib (x64)
find_library(NVAPI_LIB
    NAMES nvapi64
    PATHS "${NVAPI_INCLUDE_DIR}" "${NVAPI_INCLUDE_DIR}/../amd64"
    NO_DEFAULT_PATH)

if(NOT NVAPI_LIB)
    message(WARNING "nvapi64.lib not found — linking will fail without it.")
endif()

# ---------------------------------------------------------------------------
# DirectX / D3D9 / D3D11
# ---------------------------------------------------------------------------
find_package(DirectX QUIET)   # May not exist; fallback to SDK env
set(DXSDK_DIR "$ENV{DXSDK_DIR}" CACHE PATH "DirectX SDK root")

# Windows SDK provides d3d11; older DXSDK needed for d3d9 on some setups
# We rely on the Windows SDK being in the default include paths via MSVC.

# ---------------------------------------------------------------------------
# Runtime DLL sources
# ---------------------------------------------------------------------------
set(XR3DV_SOURCES
    src/pch.cpp
    src/main.cpp
    src/config.cpp
    src/runtime.cpp
    src/session.cpp
    src/swapchain.cpp
    src/space.cpp
    src/actions.cpp
    src/nvapi_stereo.cpp
    src/d3d9_presenter.cpp
    src/logging.cpp
    src/timing.cpp
    src/xr3dv.def        # exported symbols
)

add_library(xr3dv SHARED ${XR3DV_SOURCES})

# Precompiled header — ensures d3d11.h is always seen before openxr_platform.h
target_precompile_headers(xr3dv PRIVATE src/pch.h)

target_include_directories(xr3dv PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${NVAPI_INCLUDE_DIR}"
    "${openxr-sdk_SOURCE_DIR}/include"
    "${openxr-sdk_SOURCE_DIR}/src/common"
)

target_compile_definitions(xr3dv PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    XR3DV_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    XR3DV_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    XR3DV_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_compile_options(xr3dv PRIVATE
    /W4
    /WX-          # Warnings not errors (NVAPI headers have some)
    /permissive-
    /Zc:__cplusplus
)

target_link_libraries(xr3dv PRIVATE
    d3d9
    d3d11
    dxgi
    dxguid
    d3dcompiler
    ${NVAPI_LIB}
    shlwapi        # PathFileExists etc.
)

# ---------------------------------------------------------------------------
# Generate runtime manifest JSON next to the DLL
# ---------------------------------------------------------------------------
# Generate the OpenXR runtime manifest JSON.
# We write it to the per-config output directory so it is always next to the DLL.
# The POST_BUILD command then copies it in case the generator expression path
# differs from TARGET_FILE_DIR on some generators.
set(RUNTIME_JSON_OUT "${CMAKE_BINARY_DIR}/$<CONFIG>/xr3dv.json")

file(GENERATE OUTPUT "${RUNTIME_JSON_OUT}"
    CONTENT [=[{
    "file_format_version": "1.0.0",
    "runtime": {
        "name":                   "XR3DV",
        "library_path":           "./xr3dv.dll",
        "api_version":            "1.0.0",
        "implementation_version": "1",
        "description":            "OpenXR runtime for NVIDIA 3D Vision via NVAPI/D3D9"
    }
}
]=])

# ---------------------------------------------------------------------------
# Diagnostic tool — xr3dv_diag.exe
# ---------------------------------------------------------------------------
add_executable(xr3dv_diag src/xr3dv_diag.cpp)

target_precompile_headers(xr3dv_diag REUSE_FROM xr3dv)

target_include_directories(xr3dv_diag PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${NVAPI_INCLUDE_DIR}"
    "${openxr-sdk_SOURCE_DIR}/include"
)

target_compile_definitions(xr3dv_diag PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    XR3DV_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    XR3DV_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    XR3DV_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_compile_options(xr3dv_diag PRIVATE
    /W4 /WX- /permissive- /Zc:__cplusplus
)

target_link_libraries(xr3dv_diag PRIVATE
    d3d11
    dxgi
    shlwapi
)

# Diagnostic tool does NOT link xr3dv.dll — it loads everything dynamically.

# ---------------------------------------------------------------------------
# POST-BUILD: copy all release artifacts into the same directory as the DLL
# so that build/Release (or build/Debug) is a self-contained, ready-to-run
# release folder without needing `cmake --install`.
#
# Files copied:
#   xr3dv.dll          — runtime (built by this project)
#   xr3dv_diag.exe     — diagnostic tool (built by this project)
#   xr3dv.json         — OpenXR manifest (generated by CMake)
#   xr3dv.ini.example  — reference config
#   install.ps1        — registration script
#   uninstall.ps1      — de-registration script
#   README.md          — quick-start documentation
# ---------------------------------------------------------------------------

set(RELEASE_DIR "$<TARGET_FILE_DIR:xr3dv>")

# Copy the generated JSON manifest next to the DLL
add_custom_command(TARGET xr3dv POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RUNTIME_JSON_OUT}"
            "${RELEASE_DIR}/xr3dv.json"
    COMMENT "Copying xr3dv.json -> ${RELEASE_DIR}"
)

# Copy scripts and support files (use a helper function to keep it tidy)
foreach(_file
        "${CMAKE_SOURCE_DIR}/scripts/install.ps1"
        "${CMAKE_SOURCE_DIR}/scripts/uninstall.ps1"
        "${CMAKE_SOURCE_DIR}/config/xr3dv.ini.example"
        "${CMAKE_SOURCE_DIR}/README.md"
)

# ---------------------------------------------------------------------------
# openxr_loader.dll — locate the official pre-installed Khronos SDK DLL.
#
# Search order:
#   1. OPENXR_LOADER_DLL cmake cache variable (user override)
#   2. Next to xr3dv.dll already (user placed it manually — do nothing)
#   3. Khronos OpenXR SDK install: registry HKLM\SOFTWARE\Khronos\OpenXR\1\SDKBin
#   4. Common SDK install paths
#
# If not found, a BUILD_WARNING is issued and the DLL must be placed manually.
# Do NOT use the loader built from FetchContent — it may mismatch the target
# arch or build config and produce a "not designed to run on Windows" error.
# ---------------------------------------------------------------------------
set(OPENXR_LOADER_DLL "" CACHE FILEPATH
    "Path to official openxr_loader.dll from the Khronos OpenXR SDK installer")

if(NOT OPENXR_LOADER_DLL)
    # Try registry (Khronos SDK installer writes this)
    get_filename_component(_SDK_BIN
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Khronos\\OpenXR\\1;SDKBin]"
        ABSOLUTE)
    if(EXISTS "${_SDK_BIN}/openxr_loader.dll")
        set(OPENXR_LOADER_DLL "${_SDK_BIN}/openxr_loader.dll")
    endif()
endif()

if(NOT OPENXR_LOADER_DLL)
    # Common manual install paths
    foreach(_candidate
        "C:/Program Files/Khronos/OpenXR/bin/openxr_loader.dll"
        "C:/Program Files (x86)/Khronos/OpenXR/bin/openxr_loader.dll"
    )
        if(EXISTS "${_candidate}")
            set(OPENXR_LOADER_DLL "${_candidate}")
            break()
        endif()
    endforeach()
endif()

if(OPENXR_LOADER_DLL)
    message(STATUS "Found openxr_loader.dll: ${OPENXR_LOADER_DLL}")
    add_custom_command(TARGET xr3dv POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OPENXR_LOADER_DLL}"
                "${RELEASE_DIR}/openxr_loader.dll"
        COMMENT "Copying openxr_loader.dll -> ${RELEASE_DIR}"
    )
    install(FILES "${OPENXR_LOADER_DLL}" DESTINATION .)
else()
    message(WARNING
        "openxr_loader.dll not found. "
        "Install the Khronos OpenXR SDK from "
        "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases "
        "or set OPENXR_LOADER_DLL to the correct path and re-run CMake.")
endif()
    add_custom_command(TARGET xr3dv POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_file}"
                "${RELEASE_DIR}/"
        COMMENT "Copying ${_file} -> ${RELEASE_DIR}"
    )
endforeach()

# ---------------------------------------------------------------------------
# cmake --install support (for packagers / CI)
# ---------------------------------------------------------------------------
install(TARGETS xr3dv xr3dv_diag RUNTIME DESTINATION .)
install(FILES   "${RUNTIME_JSON_OUT}"                              DESTINATION .)
install(FILES   "${CMAKE_SOURCE_DIR}/config/xr3dv.ini.example"    DESTINATION .)
install(FILES   "${CMAKE_SOURCE_DIR}/scripts/install.ps1"         DESTINATION .)
install(FILES   "${CMAKE_SOURCE_DIR}/scripts/uninstall.ps1"       DESTINATION .)
install(FILES   "${CMAKE_SOURCE_DIR}/README.md"                   DESTINATION .)

# ---------------------------------------------------------------------------
# Convenience target: build + register in one step
# ---------------------------------------------------------------------------
add_custom_target(register_runtime
    COMMAND powershell -ExecutionPolicy Bypass -File
            "${RELEASE_DIR}/install.ps1"
            -InstallDir "${RELEASE_DIR}"
    DEPENDS xr3dv
    COMMENT "Registering XR3DV as the active OpenXR runtime"
)
