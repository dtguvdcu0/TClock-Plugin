@echo off
setlocal

:: Windows build script for MSVC with CMake.
:: Usage: build.bat [Release|Debug] [build_dir]

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

set BUILD_DIR=%2
if "%BUILD_DIR%"=="" set BUILD_DIR=build

set GENERATOR=
if not "%GENERATOR_OVERRIDE%"=="" set GENERATOR=%GENERATOR_OVERRIDE%
if "%GENERATOR%"=="" (
    for /f "delims=" %%G in ('cmake --help ^| findstr /r /c:"Visual Studio 18 2026"') do set GENERATOR=Visual Studio 18 2026
)
if "%GENERATOR%"=="" (
    for /f "delims=" %%G in ('cmake --help ^| findstr /r /c:"Visual Studio 17 2022"') do set GENERATOR=Visual Studio 17 2022
)
if "%GENERATOR%"=="" (
    echo No supported Visual Studio generator found. Set GENERATOR_OVERRIDE to override.
    exit /b 1
)
set ARCH=x64
set "SCRIPT_DIR=%~dp0"


echo Building with configuration: %CONFIG%
echo Build directory: %BUILD_DIR%

pushd "%SCRIPT_DIR%"
cmake -S . -B "%BUILD_DIR%" -G "%GENERATOR%" -A %ARCH%
if errorlevel 1 (
    echo CMake configuration failed.
    popd
    exit /b 1
)

cmake --build "%BUILD_DIR%" --config %CONFIG%
if errorlevel 1 (
    echo Build failed.
    popd
    exit /b 1
)

set "TCAP_BUILD_OUT=%BUILD_DIR%\%CONFIG%"
set "TCAP_EXE=%TCAP_BUILD_OUT%\TCapture.exe"

if not exist "%TCAP_EXE%" (
    echo ERROR: TCapture artifact not found: %TCAP_EXE%
    popd
    exit /b 1
)

echo Build succeeded: %TCAP_EXE%.
popd
exit /b 0
