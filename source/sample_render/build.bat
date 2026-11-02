@echo off
setlocal

:: Usage: build.bat [Release|Debug]
set "CS_CONFIG=%~1"
if "%CS_CONFIG%"=="" set "CS_CONFIG=Release"
if not "%~2"=="" goto USAGE
if /I "%CS_CONFIG%"=="Release" goto FIND_MSBUILD
if /I "%CS_CONFIG%"=="Debug" goto FIND_MSBUILD
goto USAGE

:FIND_MSBUILD
set "CS_MSBUILD="
if defined VSINSTALLDIR if exist "%VSINSTALLDIR%MSBuild\Current\Bin\MSBuild.exe" set "CS_MSBUILD=%VSINSTALLDIR%MSBuild\Current\Bin\MSBuild.exe"
if defined CS_MSBUILD goto BUILD
set "CS_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%CS_VSWHERE%" goto MISSING_TOOLS
for /f "usebackq delims=" %%I in (`"%CS_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "CS_MSBUILD=%%I\MSBuild\Current\Bin\MSBuild.exe"
if not defined CS_MSBUILD goto MISSING_TOOLS
if not exist "%CS_MSBUILD%" goto MISSING_TOOLS

:BUILD
"%CS_MSBUILD%" "%~dp0sample_render.vcxproj" /nologo /m /nr:false /t:Build "/p:Configuration=%CS_CONFIG%;Platform=x64"
exit /b %ERRORLEVEL%

:MISSING_TOOLS
echo ERROR: Visual Studio with MSBuild, v145 C++ tools and a Windows SDK is required.
exit /b 1

:USAGE
echo Usage: build.bat [Release^|Debug]
exit /b 2
