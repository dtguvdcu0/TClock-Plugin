@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
set "PLATFORM=%~2"
if "%PLATFORM%"=="" set "PLATFORM=x64"
if not "%~3"=="" (
    echo ERROR: Usage: build.bat [Release] [x64]
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build\build.ps1" -Configuration "%CONFIG%" -Platform "%PLATFORM%"
exit /b %errorlevel%
