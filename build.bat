@echo off
:: ============================================================================
::  build.bat  -  Build SuperVrExport and GeoVrExport addons
::
::  SuperVrExport  — SuperDepth3D games (SD3D + 3DToElse + full FS pipeline)
::  GeoVrExport    — Geo3D / native frame-sequential games (3DToElse only)
::
::  Requirements: Visual Studio 2019 or 2022 with C++ Desktop workload
::
::  Output:
::    bin\reshade_6.3.x\SuperVrExport.addon32 / .addon64
::    bin\reshade_6.3.x\GeoVrExport.addon32   / .addon64
::    bin\reshade_latest\SuperVrExport.addon32 / .addon64
::    bin\reshade_latest\GeoVrExport.addon32   / .addon64
::
::  .addon64 = 64-bit games.  .addon32 = 32-bit games.
:: ============================================================================

setlocal enabledelayedexpansion

:: ---- Locate vswhere --------------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( echo ERROR: vswhere.exe not found. & pause & exit /b 1 )

for /f "usebackq tokens=*" %%i in (
    `"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`
) do set "VSINSTALL=%%i"
if not defined VSINSTALL ( echo ERROR: No compatible Visual Studio found. & pause & exit /b 1 )

set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" ( echo ERROR: vcvarsall.bat not found. & pause & exit /b 1 )

:: ---- Paths -----------------------------------------------------------------
set "ROOT=%~dp0"
set "SRC_SUPER=%ROOT%VRExport"
set "SRC_GEO=%ROOT%GeoVrExport"
set "BINDIR=%ROOT%bin"

if not exist "%SRC_SUPER%\dllmain.cpp" ( echo ERROR: VRExport\dllmain.cpp not found. & pause & exit /b 1 )
if not exist "%SRC_GEO%\dllmain.cpp"   ( echo ERROR: GeoVrExport\dllmain.cpp not found. & pause & exit /b 1 )

:: ---- ReShade headers -------------------------------------------------------
set "INC_633=%ROOT%include_v6.3.3"
set "INC_LATEST=%ROOT%include_latest"
set "NEED_DOWNLOAD=0"
if not exist "%INC_633%\reshade.hpp"   set "NEED_DOWNLOAD=1"
if not exist "%INC_LATEST%\reshade.hpp" set "NEED_DOWNLOAD=1"

if "%NEED_DOWNLOAD%"=="0" (
    echo.
    echo  Headers already present - skipping download.
    echo  Delete include_v6.3.3\ or include_latest\ to force re-download.
    goto headers_ready
)

echo.
echo  Downloading ReShade headers for v6.3.3 (API 14)...
if not exist "%INC_633%" mkdir "%INC_633%"
set "BASE_633=https://raw.githubusercontent.com/crosire/reshade/v6.3.3/include"
for %%F in (reshade.hpp reshade_api.hpp reshade_api_device.hpp reshade_api_pipeline.hpp reshade_api_resource.hpp reshade_api_format.hpp reshade_events.hpp reshade_overlay.hpp) do (
    powershell -NoProfile -Command "Invoke-WebRequest -Uri '%BASE_633%/%%F' -OutFile '%INC_633%\%%F'" 2>nul
)
if not exist "%INC_633%\reshade.hpp" ( echo ERROR: Could not download v6.3.3 headers. & pause & exit /b 1 )

echo  Downloading ReShade headers for latest (API 16+)...
if not exist "%INC_LATEST%" mkdir "%INC_LATEST%"
set "BASE_LATEST=https://raw.githubusercontent.com/crosire/reshade/v6.4.1/include"
for %%F in (reshade.hpp reshade_api.hpp reshade_api_device.hpp reshade_api_pipeline.hpp reshade_api_resource.hpp reshade_api_format.hpp reshade_events.hpp reshade_overlay.hpp) do (
    powershell -NoProfile -Command "Invoke-WebRequest -Uri '%BASE_LATEST%/%%F' -OutFile '%INC_LATEST%\%%F'" 2>nul
)
if not exist "%INC_LATEST%\reshade.hpp" echo  WARNING: Could not download latest headers - builds 5-8 will be skipped.

:headers_ready
set "HAS_633=0"
set "HAS_LATEST=0"
if exist "%INC_633%\reshade.hpp"   set "HAS_633=1"
if exist "%INC_LATEST%\reshade.hpp" set "HAS_LATEST=1"
if "%HAS_633%"=="0" ( echo ERROR: include_v6.3.3 missing. & pause & exit /b 1 )

:: ---- Delegate to Python for the actual compile -----------------------------
:: Python handles argument quoting correctly regardless of spaces in paths.
python "%ROOT%_build_helper.py" "%VCVARS%" "%SRC_SUPER%" "%SRC_GEO%" "%BINDIR%" "%INC_633%" "%INC_LATEST%" "%HAS_LATEST%"
if errorlevel 1 ( pause & exit /b 1 )

echo.
pause
exit /b 0
