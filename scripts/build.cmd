@echo off
rem Configure + build (+ optionally test) with the newest installed MSVC toolset.
rem Usage: scripts\build.cmd [debug^|release] [test]
setlocal
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=release"

rem vcvars points VCPKG_ROOT at Visual Studio's bundled vcpkg; keep the user's own instead.
set "USER_VCPKG_ROOT=%VCPKG_ROOT%"
if not defined USER_VCPKG_ROOT for /f "tokens=2,*" %%a in ('reg query HKCU\Environment /v VCPKG_ROOT 2^>nul ^| find "VCPKG_ROOT"') do set "USER_VCPKG_ROOT=%%b"

if defined VSCMD_VER goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo error: no Visual Studio installation with the C++ toolset found
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
if defined USER_VCPKG_ROOT set "VCPKG_ROOT=%USER_VCPKG_ROOT%"
pushd "%~dp0.."
cmake --preset %PRESET% || goto :fail
cmake --build --preset %PRESET% || goto :fail
if /i "%~2"=="test" ctest --preset %PRESET% || goto :fail
popd
exit /b 0

:fail
popd
exit /b 1
