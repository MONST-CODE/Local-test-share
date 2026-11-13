@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=clang-debug"
if not "%PRESET%"=="clang-debug" if not "%PRESET%"=="clang-release" if not "%PRESET%"=="clang-ui" (
  echo Usage: build.bat [clang-debug ^| clang-release ^| clang-ui]
  exit /b 2
)
rem Microsoft SDK/STL, sans changer le compilateur clang++.
if /I "%VSCMD_ARG_TGT_ARCH%"=="x64" goto tools
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio 2022 Build Tools C++ et Windows SDK requis. Voir docs/BUILD.md.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
:tools
where clang++ >nul 2>nul
if errorlevel 1 (
  echo LLVM clang++.exe doit etre dans PATH.
  exit /b 1
)
clang++ --version
cmake --preset "%PRESET%"
if errorlevel 1 exit /b 1
cmake --build --preset "%PRESET%" --parallel
if errorlevel 1 exit /b 1
ctest --preset "%PRESET%" --output-on-failure
if errorlevel 1 exit /b 1
exit /b 0
