@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
set "PLATFORM=%~2"
if "%PLATFORM%"=="" set "PLATFORM=x64"
set "TEST_ARG="
if not "%~4"=="" (
    echo ERROR: Usage: build.bat [Release] [x64] [-Test]
    exit /b 1
)
if not "%~3"=="" if /I not "%~3"=="-Test" (
    echo ERROR: Only -Test is accepted as the third argument. Deployment is separate.
    exit /b 1
)
if /I "%~3"=="-Test" set "TEST_ARG=-Test"
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build\build.ps1" -Configuration "%CONFIG%" -Platform "%PLATFORM%" %TEST_ARG%
exit /b %errorlevel%
