@echo off
:: ============================================================================
::  build.bat  -  Build SuperVrExport.addon32 and SuperVrExport.addon64
::
::  DROP THIS FILE in the ReshadeVRExport-master folder (same level as VRExport\)
::
::  Folder structure expected:
::    VRExport\dllmain.cpp
::    VRExport\pch.cpp
::    VRExport\pch.h
::
::  Requirements:
::    Visual Studio 2019 or 2022 with C++ Desktop workload
::    (Community edition is fine)
::
::  Usage:
::    Double-click build.bat  OR  run from a plain cmd.exe prompt.
::    The script finds your VS installation automatically via vswhere.exe.
::    Headers are downloaded fresh each run - no manual setup needed.
::
::  Output:
::    bin\reshade_6.3.x\SuperVrExport.addon32   (ReShade 6.3.0 - 6.3.3, API 14)
::    bin\reshade_6.3.x\SuperVrExport.addon64
::    bin\reshade_latest\SuperVrExport.addon32  (ReShade 6.4.0+, API 16+)
::    bin\reshade_latest\SuperVrExport.addon64
::
::  Check your ReShade version: open the ReShade overlay, top-left corner.
::  Copy the matching .addon64 into your game folder (same folder as dxgi.dll).
::  Use .addon32 for 32-bit games.
:: ============================================================================

setlocal enabledelayedexpansion

:: ---- Locate vswhere --------------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
)
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found.  Please install Visual Studio 2019 or 2022.
    pause & exit /b 1
)

:: ---- Find VS install path --------------------------------------------------
for /f "usebackq tokens=*" %%i in (
    `"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`
) do set "VSINSTALL=%%i"

if not defined VSINSTALL (
    echo ERROR: No compatible Visual Studio installation found.
    pause & exit /b 1
)

set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
    echo ERROR: vcvarsall.bat not found at "%VCVARS%"
    pause & exit /b 1
)

:: ---- Paths -----------------------------------------------------------------
set "SRCDIR=%~dp0VRExport\"
set "BINDIR=%~dp0bin"

if not exist "%SRCDIR%dllmain.cpp" (
    echo ERROR: VRExport\dllmain.cpp not found. Run this from the ReshadeVRExport-master folder.
    pause & exit /b 1
)

:: ---- Download ReShade headers ----------------------------------------------
:: include_v6.3.3\  = API version 14  (ReShade 6.3.0 - 6.3.3)
:: include_latest\  = API version 16+  (ReShade 6.4.0+)
echo.
echo  Downloading ReShade headers for v6.3.3 (API 14)...
if not exist "%~dp0include_v6.3.3" mkdir "%~dp0include_v6.3.3"
set "BASE_633=https://raw.githubusercontent.com/crosire/reshade/v6.3.3/include"
for %%F in (reshade.hpp reshade_api.hpp reshade_api_device.hpp reshade_api_pipeline.hpp reshade_api_resource.hpp reshade_api_format.hpp reshade_events.hpp reshade_overlay.hpp) do (
    powershell -NoProfile -Command "Invoke-WebRequest -Uri '%BASE_633%/%%F' -OutFile '%~dp0include_v6.3.3\%%F'" 2>nul
)

if not exist "%~dp0include_v6.3.3\reshade.hpp" (
    echo ERROR: Could not download v6.3.3 headers. Check your internet connection.
    pause & exit /b 1
)
echo  v6.3.3 headers ready.

echo  Downloading ReShade headers for latest (API 16+)...
if not exist "%~dp0include_latest" mkdir "%~dp0include_latest"
set "BASE_LATEST=https://raw.githubusercontent.com/crosire/reshade/v6.4.1/include"
for %%F in (reshade.hpp reshade_api.hpp reshade_api_device.hpp reshade_api_pipeline.hpp reshade_api_resource.hpp reshade_api_format.hpp reshade_events.hpp reshade_overlay.hpp) do (
    powershell -NoProfile -Command "Invoke-WebRequest -Uri '%BASE_LATEST%/%%F' -OutFile '%~dp0include_latest\%%F'" 2>nul
)

set "HAS_LATEST=0"
if exist "%~dp0include_latest\reshade.hpp" set "HAS_LATEST=1"
if "%HAS_LATEST%"=="1" (
    echo  Latest headers ready.
) else (
    echo  WARNING: Could not download latest headers - will skip builds 3+4.
    echo  To build for newer ReShade manually place headers in include_latest\
)
echo.

:: ---- Common compiler flags -------------------------------------------------
:: /DUNICODE + /D_UNICODE so CreateFileMapping resolves to CreateFileMappingW
:: matching the L"Local\\KatangaMappedFile" wide string literals in dllmain.cpp
set "CFLAGS=/nologo /std:c++17 /O2 /GL /EHsc /W3 /DWIN32 /D_WINDOWS /DNDEBUG /DUNICODE /D_UNICODE"
set "LFLAGS=/DLL /OPT:REF /OPT:ICF /LTCG /INCREMENTAL:NO"
set "LIBS=kernel32.lib user32.lib d3d9.lib d3d10.lib d3d11.lib d3d12.lib dxgi.lib opengl32.lib"

:: ============================================================================
::  Build for ReShade 6.3.x  (API version 14)  - most common installed version
:: ============================================================================
echo.
echo ============================================================
echo  Build 1/4: x86 for ReShade 6.3.x (API 14)
echo ============================================================
set "OUTDIR=%BINDIR%\reshade_6.3.x"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
call :build_arch x86 "%OUTDIR%\SuperVrExport.addon32" "%~dp0include_v6.3.3"
if errorlevel 1 ( echo ERROR: Build 1 failed. & pause & exit /b 1 )

echo.
echo ============================================================
echo  Build 2/4: x64 for ReShade 6.3.x (API 14)
echo ============================================================
call :build_arch x64 "%OUTDIR%\SuperVrExport.addon64" "%~dp0include_v6.3.3"
if errorlevel 1 ( echo ERROR: Build 2 failed. & pause & exit /b 1 )

:: ============================================================================
::  Build for ReShade latest  (API version 16+)  - skipped if headers missing
:: ============================================================================
if "%HAS_LATEST%"=="0" goto skip_latest

echo.
echo ============================================================
echo  Build 3/4: x86 for ReShade latest (API 16+)
echo ============================================================
set "OUTDIR=%BINDIR%\reshade_latest"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
call :build_arch x86 "%OUTDIR%\SuperVrExport.addon32" "%~dp0include_latest"
if errorlevel 1 ( echo ERROR: Build 3 failed. & pause & exit /b 1 )

echo.
echo ============================================================
echo  Build 4/4: x64 for ReShade latest (API 16+)
echo ============================================================
call :build_arch x64 "%OUTDIR%\SuperVrExport.addon64" "%~dp0include_latest"
if errorlevel 1 ( echo ERROR: Build 4 failed. & pause & exit /b 1 )

:skip_latest

echo.
echo ============================================================
echo  ALL BUILDS SUCCESSFUL
echo.
echo  For ReShade 6.3.0 - 6.3.3:  bin\reshade_6.3.x\SuperVrExport.addon64
echo  For ReShade 6.4.0+         :  bin\reshade_latest\SuperVrExport.addon64  (if built)
echo.
echo  Check your ReShade version: open ReShade overlay, top-left corner.
echo  Copy the matching .addon64 into your game folder (same folder as dxgi.dll).
echo ============================================================
echo.
pause
exit /b 0

:: ============================================================================
::  Subroutine :build_arch  arch  output_file  include_dir
:: ============================================================================
:build_arch
setlocal
set "ARCH=%~1"
set "OUTFILE=%~2"
set "HDR=%~3"

call "%VCVARS%" %ARCH%
if errorlevel 1 ( echo ERROR: vcvarsall failed & endlocal & exit /b 1 )

:: CD into source dir so filenames have no spaces - paths only needed for includes/output
pushd "%SRCDIR%"

:: Add Vulkan SDK headers if available, and define SUPVR_VULKAN=1 to enable Vulkan code path
set "VULKAN_INC="
set "VULKAN_DEF=/DSUPVR_VULKAN=0"
if defined VULKAN_SDK (
    set "VULKAN_INC=/I"%VULKAN_SDK%\Include""
    set "VULKAN_DEF=/DSUPVR_VULKAN=1"
)

cl %CFLAGS% /I"%HDR%" /I"." %VULKAN_INC% %VULKAN_DEF% dllmain.cpp pch.cpp /link %LFLAGS% %LIBS% /OUT:"%OUTFILE%"

set BRET=%errorlevel%
popd
endlocal & exit /b %BRET%
