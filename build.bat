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
::    bin\reshade_6.3.x\SuperVrExport.addon32 / .addon64   (ReShade 6.3.x, API 14)
::    bin\reshade_6.3.x\GeoVrExport.addon32   / .addon64
::    bin\reshade_6.4.x\SuperVrExport.addon32 / .addon64   (ReShade 6.4.x, API 16)
::    bin\reshade_6.4.x\GeoVrExport.addon32   / .addon64
::    bin\reshade_6.7.x\SuperVrExport.addon32 / .addon64   (ReShade 6.7.x, API 16+)
::    bin\reshade_6.7.x\GeoVrExport.addon32   / .addon64
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
set "INC_64X=%ROOT%include_v6.4.x"
set "INC_67X=%ROOT%include_v6.7.x"
set "NEED_DOWNLOAD=0"
if not exist "%INC_633%\reshade.hpp" set "NEED_DOWNLOAD=1"
if not exist "%INC_64X%\reshade.hpp" set "NEED_DOWNLOAD=1"
if not exist "%INC_67X%\reshade.hpp" set "NEED_DOWNLOAD=1"

if "%NEED_DOWNLOAD%"=="0" (
    echo.
    echo  Headers already present - skipping download.
    echo  Delete include_v6.3.3\, include_v6.4.x\, or include_v6.7.x\ to force re-download.
    goto headers_ready
)

set "FILES=reshade.hpp reshade_api.hpp reshade_api_device.hpp reshade_api_pipeline.hpp reshade_api_resource.hpp reshade_api_format.hpp reshade_events.hpp reshade_overlay.hpp"

echo.
echo  Downloading ReShade headers for v6.3.3 (API 14)...
if not exist "%INC_633%" mkdir "%INC_633%"
set "BASE=https://raw.githubusercontent.com/crosire/reshade/v6.3.3/include"
for %%F in (%FILES%) do (
    powershell -NoProfile -Command "Invoke-WebRequest -Uri '%BASE%/%%F' -OutFile '%INC_633%\%%F'" 2>nul
)
if not exist "%INC_633%\reshade.hpp" ( echo ERROR: Could not download v6.3.3 headers. & pause & exit /b 1 )

echo  Downloading ReShade headers for v6.4.1 (API 16)...
if not exist "%INC_64X%" mkdir "%INC_64X%"
set "BASE=https://raw.githubusercontent.com/crosire/reshade/v6.4.1/include"
for %%F in (%FILES%) do (
    powershell -NoProfile -Command "Invoke-WebRequest -Uri '%BASE%/%%F' -OutFile '%INC_64X%\%%F'" 2>nul
)
if not exist "%INC_64X%\reshade.hpp" echo  WARNING: Could not download v6.4.1 headers - those builds will be skipped.

echo  Downloading ReShade headers for v6.7.3 (API 16+)...
if not exist "%INC_67X%" mkdir "%INC_67X%"
set "BASE=https://raw.githubusercontent.com/crosire/reshade/v6.7.3/include"
for %%F in (%FILES%) do (
    powershell -NoProfile -Command "Invoke-WebRequest -Uri '%BASE%/%%F' -OutFile '%INC_67X%\%%F'" 2>nul
)
if not exist "%INC_67X%\reshade.hpp" echo  WARNING: Could not download v6.7.3 headers - those builds will be skipped.

:headers_ready
set "HAS_633=0"
set "HAS_64X=0"
set "HAS_67X=0"
if exist "%INC_633%\reshade.hpp" set "HAS_633=1"
if exist "%INC_64X%\reshade.hpp" set "HAS_64X=1"
if exist "%INC_67X%\reshade.hpp" set "HAS_67X=1"
if "%HAS_633%"=="0" ( echo ERROR: include_v6.3.3 missing. & pause & exit /b 1 )

:: ---- Delegate to Python for the actual compile -----------------------------
python "%ROOT%_build_helper.py" "%VCVARS%" "%SRC_SUPER%" "%SRC_GEO%" "%BINDIR%" "%INC_633%" "%INC_64X%" "%INC_67X%" "%HAS_64X%" "%HAS_67X%"
if errorlevel 1 ( pause & exit /b 1 )

echo.
pause
exit /b 0
