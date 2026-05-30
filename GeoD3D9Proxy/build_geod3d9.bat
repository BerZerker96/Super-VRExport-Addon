@echo off
:: ===========================================================================
::  build_geod3d9.bat  -  Build geod3d9.dll (D3D9 -> D3D9Ex upgrading proxy)
::
::  Dragon Age: Origins is 32-bit, so this builds x86 by default.
::  Run from an "x86 Native Tools Command Prompt for VS", or just double-click
::  (it will try to locate vcvarsall and set up the x86 environment).
::
::  Output:  GeoD3D9Proxy\geod3d9.dll   (32-bit)
:: ===========================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

:: --- Ensure the x86 compiler is on PATH ------------------------------------
where cl.exe >nul 2>&1
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    )
    if defined VSPATH (
        echo Initializing x86 build environment...
        call "!VSPATH!\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
    ) else (
        echo ERROR: Could not find Visual Studio. Open an "x86 Native Tools Command Prompt" and re-run.
        pause & exit /b 1
    )
)

:: --- Compile ----------------------------------------------------------------
:: /LD  build a DLL   /MT static CRT (no vcruntime dependency in the game folder)
:: /O2  optimize      /EHsc C++ exceptions   /GS- tiny, no need for stack cookies here
cl /nologo /LD /O2 /EHsc /MT /DUNICODE /D_UNICODE dllmain.cpp ^
   /link /DEF:geod3d9.def /MACHINE:X86 /OUT:geod3d9.dll

if errorlevel 1 ( echo. & echo BUILD FAILED & pause & exit /b 1 )

del /q dllmain.obj geod3d9.exp geod3d9.lib 2>nul
echo.
echo Built: %~dp0geod3d9.dll  (32-bit)
echo.
echo Install:
echo   1. Move ReShade's d3d9.dll into a subfolder:  ^<game^>\ReShade\d3d9.dll
echo   2. Copy geod3d9.dll into the game folder and rename it to  d3d9.dll
echo   3. (optional) create geod3d9.ini to point at a different ReShade path
pause
