@echo off
rem Configures and builds Deskflower with MSVC + Ninja. Usage: scripts\build-windows.bat [win-debug|win-release]
setlocal
set PRESET=%1
if "%PRESET%"=="" set PRESET=win-debug
rem Header dependency tracking: Ninja matches the /showIncludes prefix byte by byte. cl prints it in
rem UTF-8 while CMake writes rules.ninja in the console codepage, so localized compilers (e.g. Spanish
rem "Nota: inclusion del archivo") never match. A UTF-8 console makes both sides agree.
chcp 65001 >nul

if defined VSCMD_VER goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
set "PATH=C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;%PATH%"
cd /d "%~dp0.."
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
