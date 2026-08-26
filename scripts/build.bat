@echo off
setlocal enabledelayedexpansion
REM Builds printer_agent.exe from the command line - no CLion required, just
REM the same MSVC toolchain CLion uses (Visual Studio 2022 Build Tools with
REM the "Desktop development with C++" workload).
REM
REM Usage:
REM   scripts\build.bat            builds Release into cmake-build-release\
REM   scripts\build.bat Debug      builds Debug into cmake-build-debug\
REM
REM Uses the same generator/output directories as CLion's own CMake profiles
REM (see .idea\workspace.xml) so running this script and building from CLion
REM stay in sync instead of fighting over the same directory with two
REM different generators.

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
if /I not "%CONFIG%"=="Debug" if /I not "%CONFIG%"=="Release" (
    echo ERROR: unknown config "%CONFIG%" - expected "Debug" or "Release".
    exit /b 1
)

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

if /I "%CONFIG%"=="Debug" (
    set "BUILD_DIR=%ROOT%\cmake-build-debug"
) else (
    set "BUILD_DIR=%ROOT%\cmake-build-release"
)

echo === Printer Inventory Agent - %CONFIG% build ===
echo Project root: %ROOT%
echo Build dir:    %BUILD_DIR%
echo.

REM --- Locate cmake.exe: PATH first, then CLion's bundled copy ---
set "CMAKE_EXE="
where cmake >nul 2>&1
if %errorlevel%==0 (
    set "CMAKE_EXE=cmake"
) else (
    if exist "%LOCALAPPDATA%\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe" (
        set "CMAKE_EXE=%LOCALAPPDATA%\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe"
    )
)
if "%CMAKE_EXE%"=="" (
    echo ERROR: cmake.exe not found on PATH and no CLion-bundled copy was found.
    echo Install CMake ^(cmake.org^), or open this project once in CLion so it
    echo downloads its bundled copy.
    exit /b 1
)
echo Using CMake: %CMAKE_EXE%

REM --- Locate ninja.exe: PATH first, then CLion's bundled copy ---
set "NINJA_EXE="
where ninja >nul 2>&1
if %errorlevel%==0 (
    set "NINJA_EXE=ninja"
) else (
    if exist "%LOCALAPPDATA%\Programs\CLion\bin\ninja\win\x64\ninja.exe" (
        set "NINJA_EXE=%LOCALAPPDATA%\Programs\CLion\bin\ninja\win\x64\ninja.exe"
    )
)
if "%NINJA_EXE%"=="" (
    echo ERROR: ninja.exe not found on PATH and no CLion-bundled copy was found.
    echo Install Ninja ^(ninja-build.org^), or open this project once in CLion so
    echo it downloads its bundled copy.
    exit /b 1
)
echo Using Ninja: %NINJA_EXE%

REM --- Locate Visual Studio / Build Tools 2022 (MSVC) via vswhere ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found - is Visual Studio / Build Tools 2022 installed?
    exit /b 1
)

set "VSINSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALL=%%I"
)
if "%VSINSTALL%"=="" (
    echo ERROR: No Visual Studio install with the C++ ^(VC.Tools^) workload was found.
    echo Install "Desktop development with C++" via the Visual Studio Installer,
    echo or run: winget install --id Microsoft.VisualStudio.2022.BuildTools -e ^
        --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools"
    exit /b 1
)
echo Using Visual Studio at: %VSINSTALL%

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed to set up the x64 MSVC environment.
    exit /b 1
)

echo.
echo --- Configuring ---
"%CMAKE_EXE%" -G Ninja -S "%ROOT%" -B "%BUILD_DIR%" -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 (
    echo ERROR: CMake configure failed.
    exit /b 1
)

echo.
echo --- Building ---
"%CMAKE_EXE%" --build "%BUILD_DIR%" --target printer_agent
if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

echo.
echo === Build succeeded: %BUILD_DIR%\printer_agent.exe ===
echo agent.ini and scripts\install_service.bat were copied alongside it -
echo that folder is ready to copy to a target machine and run install_service.bat.
