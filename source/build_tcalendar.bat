@echo off
setlocal

set "TCAL_BUILD=%~dp0tcal\build.bat"
if not exist "%TCAL_BUILD%" (
    echo ERROR: TCalendar build script not found: %TCAL_BUILD%
    exit /b 1
)

echo Building TCalendar...
call "%TCAL_BUILD%" Release build-vs
if errorlevel 1 (
    echo ERROR: TCalendar build failed.
    exit /b 1
)

echo TCalendar build completed.
exit /b 0
