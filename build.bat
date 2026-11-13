@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem PLPT authoritative Windows build. Direct clang++ / lld-link / SDK tools only.
cd /d "%~dp0"
set "ROOT=%CD%"
set "ACTION=%~1"
if not defined ACTION set "ACTION=debug"
if not "%~1"=="" shift
set "MODE=debug"
set "VERBOSE=0"
set "CORE_ONLY=0"
set "FETCH=0"
set "NATIVE_UI=0"
:arguments
if "%~1"=="" goto arguments_done
if /I "%~1"=="--verbose" (set "VERBOSE=1"& shift& goto arguments)
if /I "%~1"=="--core-only" (set "CORE_ONLY=1"& shift& goto arguments)
if /I "%~1"=="--fetch" (set "FETCH=1"& shift& goto arguments)
if /I "%~1"=="--native-ui" (set "NATIVE_UI=1"& shift& goto arguments)
if /I "%ACTION%"=="test" if /I "%~1"=="debug" (set "MODE=debug"& shift& goto arguments)
if /I "%ACTION%"=="test" if /I "%~1"=="release" (set "MODE=release"& shift& goto arguments)
if /I "%ACTION%"=="test" if /I "%~1"=="asan" (set "MODE=asan"& shift& goto arguments)
call :problem "unknown argument %~1"
exit /b 2
:arguments_done
set "VALID="
for %%A in (debug release test asan fuzz clean package doctor deps selftest help) do if /I "%ACTION%"=="%%A" set "VALID=1"
if not defined VALID (call :problem "unknown operation %ACTION%"& exit /b 2)
if /I "%ACTION%"=="help" goto help
if /I not "%PROCESSOR_ARCHITECTURE%"=="AMD64" (call :problem "native Windows x64 command prompt required"& exit /b 2)
if not exist "engine\core\types.hpp" (call :problem "wrong checkout"& exit /b 2)
set "OUT=%ROOT%\out\windows"
set "DEPS=%OUT%\dependencies"
for %%D in ("%ROOT%\out" "%OUT%" "%OUT%\logs" "%OUT%\dependencies" "%OUT%\debug" "%OUT%\release" "%OUT%\tests" "%OUT%\packages" "%OUT%\doctor") do (
  call :plain_directory "%%~D" || exit /b
)
if not exist "%OUT%\logs" mkdir "%OUT%\logs"
if not exist "%OUT%\logs" (call :problem "cannot create logs"& exit /b 1)
set "LOG=%OUT%\logs\%ACTION%-%MODE%-%RANDOM%-%RANDOM%.log"
set "CMDLOG=%LOG%.command.txt"
>"%LOG%" echo PLPT direct Windows operation=%ACTION% mode=%MODE% started=%DATE% %TIME%
mkdir "%OUT%\.build-lock" >nul 2>nul
if not "%errorlevel%"=="0" (call :problem "another build owns %OUT%\.build-lock. Remove only after verifying no build is running."& exit /b 1)
set "LOCK=1"
set "SDL_SHA=1ec3009655ee37b1f73e52b3b120acc5afd8abaf9532f3303b1bf1dfe91b5969"
set "IMGUI_SHA=b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114"
set "SDL_ARCHIVE=%DEPS%\SDL3-devel-3.4.20-VC.zip"
set "IMGUI_ARCHIVE=%DEPS%\imgui-c1aaac65e896a45f81f47a4d68f9959e0f86c859.tar.gz"
rem Explicit project sources. Models never enter installed monitor/desktop targets.
set "CORE_SOURCES=engine/core/random.cpp engine/diagnostics/audit.cpp engine/discovery/process_tree.cpp engine/discovery/terminal_classifier.cpp engine/discovery/monitor.cpp engine/service_registry/registry.cpp engine/port_allocator/port_allocator.cpp engine/platform/windows/windows_platform.cpp"
set "MODEL_SOURCES=engine/session/session.cpp engine/messaging/messaging.cpp engine/protocol/envelope.cpp engine/resource_manager/resource.cpp"
set "IMGUI_SOURCES=imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp backends/imgui_impl_sdl3.cpp backends/imgui_impl_sdlrenderer3.cpp"
set "HEADERS=engine/browser/browser.hpp engine/core/clock.hpp engine/core/random.hpp engine/core/result.hpp engine/core/text.hpp engine/core/types.hpp engine/diagnostics/audit.hpp engine/discovery/monitor.hpp engine/discovery/process_tree.hpp engine/discovery/terminal_classifier.hpp engine/messaging/messaging.hpp engine/platform/native.hpp engine/platform/platform.hpp engine/port_allocator/port_allocator.hpp engine/protocol/envelope.hpp engine/resource_manager/resource.hpp engine/service_registry/registry.hpp engine/session/session.hpp engine/transport/transport.hpp engine/platform/windows/windows_platform.hpp engine/platform/windows/winsock.hpp"
call :dispatch
set "FINALRC=%errorlevel%"
if "%LOCK%"=="1" rmdir "%OUT%\.build-lock"
if "%FINALRC%"=="0" (
  echo SUCCESS operation=%ACTION%. Log: %LOG%
  >>"%LOG%" echo SUCCESS operation=%ACTION%
) else (
  echo FAILURE operation=%ACTION% exit=%FINALRC%. Log: %LOG%
  >>"%LOG%" echo FAILURE operation=%ACTION% exit=%FINALRC%
  if "%GITHUB_ACTIONS%"=="true" if not "%PLPT_EXPECT_FAILURE%"=="1" echo ::error title=PLPT Windows::Operation %ACTION% failed, exit %FINALRC%. See retained logs.
)
endlocal & exit /b %FINALRC%

:help
echo Usage: build.bat debug^|release^|test [debug^|release^|asan]^|asan^|fuzz^|clean^|package^|doctor^|deps^|selftest [--verbose] [--core-only] [--native-ui]
echo deps --fetch explicitly downloads pinned archives. No other operation downloads.
echo Windows ASan/fuzzer require LLVM compiler-rt; missing support returns SKIP code 77, never PASS.
exit /b 0

:dispatch
if /I "%ACTION%"=="deps" goto dependencies_action
if /I "%ACTION%"=="clean" goto clean_action
if /I "%ACTION%"=="doctor" goto doctor_action
if /I "%ACTION%"=="selftest" goto selftest
if /I "%ACTION%"=="fuzz" goto fuzz
if /I "%ACTION%"=="package" goto package
if /I "%ACTION%"=="debug" set "MODE=debug"
if /I "%ACTION%"=="release" set "MODE=release"
set "WITH_TESTS=0"
set "DIR=%OUT%\%MODE%"
if /I "%ACTION%"=="test" (set "WITH_TESTS=1"& set "DIR=%OUT%\tests\%MODE%")
if /I "%ACTION%"=="asan" (set "MODE=asan"& set "WITH_TESTS=1"& set "DIR=%OUT%\tests\asan")
call :build_project || exit /b
if "%WITH_TESTS%"=="1" call :tests || exit /b
exit /b 0

:doctor_action
call :toolchain || exit /b
goto dependencies

:plain_directory
rem Reject reparse points before any recursive cleanup; all callers use fixed checkout descendants.
if not exist "%~1" exit /b 0
set "ATTR="
for %%I in ("%~1") do set "ATTR=%%~aI"
echo %ATTR% | findstr /I "l" >nul
if "%errorlevel%"=="0" (call :problem "reparse-point output is not allowed: %~1"& exit /b 1)
exit /b 0

:fresh
call :plain_directory "%~1" || exit /b
if exist "%~1" rmdir /s /q "%~1"
if exist "%~1" (call :problem "cannot remove old outputs: %~1"& exit /b 1)
mkdir "%~1"
if not exist "%~1" (call :problem "cannot create output: %~1"& exit /b 1)
exit /b 0

:run
rem Capture each process without a pipeline masking its status. Print diagnostics afterwards.
>>"%LOG%" echo + %*
if "%VERBOSE%"=="1" echo + %*
%* >"%CMDLOG%" 2>&1
set "RC=%errorlevel%"
type "%CMDLOG%"
type "%CMDLOG%" >>"%LOG%"
if not "%errorlevel%"=="0" (call :problem "cannot preserve command log"& exit /b 1)
if not "%RC%"=="0" (
  echo Command exit %RC%: %*
  >>"%LOG%" echo Command exit %RC%: %*
  if "%GITHUB_ACTIONS%"=="true" if not "%PLPT_EXPECT_FAILURE%"=="1" if not "%RC%"=="%RUN_EXPECTED%" (
    findstr /I /C:"error:" /C:"fatal" /C:"undefined" /C:"FAIL" "%CMDLOG%" >"%LOG%.diagnostics.txt"
    call :notice_file "%LOG%.diagnostics.txt"
  )
)
exit /b %RC%

:problem
echo ERROR: %~1
if defined LOG >>"%LOG%" echo ERROR: %~1
if "%GITHUB_ACTIONS%"=="true" if not "%PLPT_EXPECT_FAILURE%"=="1" echo ::error title=PLPT Windows validation::%~1
exit /b 0

:notice_file
if not "%GITHUB_ACTIONS%"=="true" exit /b 0
rem One bounded multiline annotation avoids GitHub's per-step annotation limit.
setlocal EnableDelayedExpansion
set "NOTE="
for /f "usebackq delims=" %%L in ("%~1") do (
  set "LINE=%%L"
  set "LINE=!LINE:%%=%%25!"
  set "LINE=!LINE:~0,1000!"
  set "NOTE=!NOTE!!LINE!%%0A"
  if not "!NOTE:~6500!"=="" goto notice_emit
)
:notice_emit
if not defined NOTE (endlocal& exit /b 0)
set "NOTE=!NOTE:~0,7000!"
echo ::notice title=PLPT Windows evidence::!NOTE!
endlocal
exit /b 0

:toolchain
if /I "%VSCMD_ARG_TGT_ARCH%"=="x64" goto sdk_ready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (call :problem "Visual Studio C++ Build Tools and Windows SDK required"& exit /b 2)
set "VSROOT="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT (call :problem "no compatible C++ Build Tools installation"& exit /b 2)
rem Environment initialization only. No project generator or other build engine is invoked.
call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >"%CMDLOG%" 2>&1
set "RC=%errorlevel%"
type "%CMDLOG%"
type "%CMDLOG%" >>"%LOG%"
if not "%RC%"=="0" exit /b %RC%
:sdk_ready
if /I not "%VSCMD_ARG_TGT_ARCH%"=="x64" (call :problem "SDK environment must target x64"& exit /b 2)
if not defined WindowsSdkDir (call :problem "WindowsSdkDir missing"& exit /b 2)
if not defined WindowsSDKVersion (call :problem "WindowsSDKVersion missing"& exit /b 2)
if not defined VCToolsInstallDir (call :problem "VCToolsInstallDir missing"& exit /b 2)
if not defined INCLUDE (call :problem "required SDK/STL INCLUDE paths missing"& exit /b 2)
if not defined LIB (call :problem "required SDK/STL LIB paths missing"& exit /b 2)
for %%F in ("%VCToolsInstallDir%include\vector" "%VCToolsInstallDir%lib\x64\libcmt.lib" "%VCToolsInstallDir%lib\x64\libcmtd.lib" "%WindowsSdkDir%Include\%WindowsSDKVersion%um\Windows.h" "%WindowsSdkDir%Include\%WindowsSDKVersion%ucrt\stdlib.h" "%WindowsSdkDir%Lib\%WindowsSDKVersion%um\x64\kernel32.lib" "%WindowsSdkDir%Lib\%WindowsSDKVersion%ucrt\x64\libucrt.lib") do (
  if not exist "%%~F" (call :problem "required SDK/STL file missing: %%~F"& exit /b 2)
)
set "CXX="
if defined PLPT_CXX set "CXX=%PLPT_CXX%"
rem Prefer the official LLVM installation; VsDevCmd prepends its own bundled LLVM.
if not defined CXX if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "CXX=%ProgramFiles%\LLVM\bin\clang++.exe"
if not defined CXX for /f "delims=" %%I in ('where clang++.exe 2^>nul') do if not defined CXX set "CXX=%%I"
if not defined CXX (call :problem "LLVM clang++.exe not found"& exit /b 2)
if not exist "%CXX%" (call :problem "selected compiler does not exist: %CXX%"& exit /b 2)
for %%I in ("%CXX%") do set "CXXNAME=%%~nxI"
if /I not "%CXXNAME%"=="clang++.exe" (call :problem "choose clang++.exe, not another driver"& exit /b 2)
for %%I in ("%CXX%") do set "LLD=%%~dpIlld-link.exe"
if not exist "%LLD%" (call :problem "matching lld-link.exe required next to clang++.exe"& exit /b 2)
call :run "%CXX%" --version || exit /b
if "%GITHUB_ACTIONS%"=="true" for %%I in ("%CMDLOG%") do echo ::notice title=PLPT compiler audit::compiler=%CXX% output=%%~fI bytes=%%~zI
for /f "usebackq delims=" %%L in ("%CMDLOG%") do if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT compiler version::%%L
findstr /I /C:"clang version" "%CMDLOG%" >nul || (call :problem "selected compiler is not LLVM Clang: %CXX%"& exit /b 2)
call :run "%CXX%" -dumpmachine || exit /b
findstr /I /C:"x86_64-pc-windows-msvc" "%CMDLOG%" >nul || (call :problem "compiler default target must be x86_64-pc-windows-msvc. MinGW is forbidden."& exit /b 2)
call :run "%CXX%" -dumpversion || exit /b
set "CLANG_MAJOR="
for /f "usebackq tokens=1 delims=." %%V in ("%CMDLOG%") do set "CLANG_MAJOR=%%V"
if not defined CLANG_MAJOR (call :problem "cannot determine Clang version"& exit /b 2)
if %CLANG_MAJOR% LSS 18 (call :problem "LLVM Clang 18 or newer required; SDK compatibility is probed natively"& exit /b 2)
call :run "%LLD%" --version || exit /b
set "RCEXE="
set "DUMPBIN="
set "MTEXE="
for /f "delims=" %%I in ('where rc.exe 2^>nul') do if not defined RCEXE set "RCEXE=%%I"
for /f "delims=" %%I in ('where dumpbin.exe 2^>nul') do if not defined DUMPBIN set "DUMPBIN=%%I"
for /f "delims=" %%I in ('where mt.exe 2^>nul') do if not defined MTEXE set "MTEXE=%%I"
if not defined RCEXE (call :problem "Windows SDK rc.exe missing"& exit /b 2)
if not defined MTEXE (call :problem "Windows SDK mt.exe missing"& exit /b 2)
if not defined DUMPBIN (call :problem "MSVC dumpbin.exe missing"& exit /b 2)
>>"%LOG%" echo SDK=%WindowsSdkDir% version=%WindowsSDKVersion% VC=%VCToolsInstallDir%
>>"%LOG%" echo INCLUDE=%INCLUDE%
>>"%LOG%" echo LIB=%LIB%
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT Windows SDK::SDK=%WindowsSDKVersion% VC=%VCToolsVersion% compiler=%CXX% linker=%LLD%
if not exist "%OUT%\doctor" mkdir "%OUT%\doctor"
call :run "%CXX%" --target=x86_64-pc-windows-msvc -std=c++20 -fms-runtime-lib=static -fuse-ld=lld -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -Wall -Wextra -Wpedantic -Werror tests/build/toolchain_probe.cpp -o "%OUT%\doctor\probe.exe" -lkernel32 -lbcrypt || exit /b
if not exist "%OUT%\doctor\probe.exe" (call :problem "toolchain probe output missing"& exit /b 1)
call :run "%OUT%\doctor\probe.exe" || exit /b
call :notice_file "%CMDLOG%"
exit /b 0

:verify
if not exist "%~1" (call :problem "missing archive %~1. Run build.bat deps --fetch or provision that exact pinned file."& exit /b 2)
call :run certutil.exe -hashfile "%~1" SHA256 || exit /b
findstr /I /X /C:"%~2" "%CMDLOG%" >nul
if not "%errorlevel%"=="0" (call :problem "SHA-256 mismatch for %~1. Archive NOT extracted."& exit /b 1)
echo SHA256 OK %~2 %~1
>>"%LOG%" echo SHA256 OK %~2 %~1
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT Windows dependency::SHA256 %~2 %~nx1
exit /b 0

:dependencies
if "%CORE_ONLY%"=="1" exit /b 0
call :verify "%SDL_ARCHIVE%" "%SDL_SHA%" || exit /b
call :verify "%IMGUI_ARCHIVE%" "%IMGUI_SHA%" || exit /b
exit /b 0

:dependencies_action
if not exist "%DEPS%" mkdir "%DEPS%"
if "%FETCH%"=="1" (
  call :fetch_one "https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-devel-3.4.20-VC.zip" "%SDL_ARCHIVE%" "%SDL_SHA%" || exit /b
  call :fetch_one "https://codeload.github.com/ocornut/imgui/tar.gz/c1aaac65e896a45f81f47a4d68f9959e0f86c859" "%IMGUI_ARCHIVE%" "%IMGUI_SHA%" || exit /b
)
goto dependencies

:fetch_one
if exist "%~2" (call :verify "%~2" "%~3"& exit /b)
call :run curl.exe --fail --location --show-error --proto "=https" --tlsv1.2 --output "%~2.part" "%~1" || exit /b
call :verify "%~2.part" "%~3" || exit /b
move /y "%~2.part" "%~2" >nul
exit /b %errorlevel%

:stamp
for /f "tokens=1-4 delims=:., " %%a in ("%TIME: =0%") do set /a "STAMP=((1%%a-100)*360000)+((1%%b-100)*6000)+((1%%c-100)*100)+(1%%d-100)" >nul
exit /b 0

:configure_flags
set "PROFILE_FLAGS=-O0 -g -fms-runtime-lib=static_dbg"
set "LINK_FLAGS=-fms-runtime-lib=static_dbg -Xlinker /DEBUG"
set "SAN="
if "%MODE%"=="release" (
  set "PROFILE_FLAGS=-O2 -DNDEBUG -fms-runtime-lib=static"
  set "LINK_FLAGS=-fms-runtime-lib=static"
)
if "%MODE%"=="asan" (
  set "PROFILE_FLAGS=-O1 -g -fms-runtime-lib=static"
  set "LINK_FLAGS=-fms-runtime-lib=static -Xlinker /DEBUG"
  set "SAN=-fsanitize=address -fno-omit-frame-pointer"
)
set "VFLAG="
if "%VERBOSE%"=="1" set "VFLAG=-v"
set "COMMON_FLAGS=--target=x86_64-pc-windows-msvc -std=c++20 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00"
set "WARNINGS=-Wall -Wextra -Wpedantic -Werror -Wshadow"
set "NATIVE_LIBS=-lws2_32 -liphlpapi -lbcrypt -lkernel32"
exit /b 0

:cpp
set "SOURCE=%~1"
set "OBJECT=%DIR%\objects\%SOURCE:/=_%.obj"
echo Compilation: %SOURCE%
call :run "%CXX%" %VFLAG% %COMMON_FLAGS% %PROFILE_FLAGS% %SAN% %WARNINGS% %PLPT_EXTRA_CXXFLAGS% -I "%ROOT%" %UI_INCLUDES% -MMD -MP -MF "%OBJECT%.d" -c "%SOURCE%" -o "%OBJECT%" || exit /b
if not exist "%OBJECT%" (call :problem "compiler returned success without object %OBJECT%"& exit /b 1)
exit /b 0

:core_one
call :cpp "%~1" || exit /b
set "CORE_OBJECTS=%CORE_OBJECTS% "%OBJECT%""
exit /b 0
:model_one
call :cpp "%~1" || exit /b
set "MODEL_OBJECTS=%MODEL_OBJECTS% "%OBJECT%""
exit /b 0
:gui_one
set "GUI_SOURCE=%~1"
set "GUI_OBJECT=%DIR%\objects\imgui_%GUI_SOURCE:/=_%.obj"
echo Compilation ImGui: %GUI_SOURCE%
call :run "%CXX%" %VFLAG% %COMMON_FLAGS% %PROFILE_FLAGS% %SAN% -I "%IMGUI_ROOT%" -I "%IMGUI_ROOT%\backends" -I "%SDL_ROOT%\include" -MMD -MP -MF "%GUI_OBJECT%.d" -c "%IMGUI_ROOT%\%GUI_SOURCE%" -o "%GUI_OBJECT%" || exit /b
if not exist "%GUI_OBJECT%" (call :problem "missing ImGui object %GUI_OBJECT%"& exit /b 1)
set "GUI_OBJECTS=%GUI_OBJECTS% "%GUI_OBJECT%""
exit /b 0

:link
rem LINK_OBJECTS is an explicit target set, never a directory wildcard.
echo Linking: %~1
call :run "%CXX%" %VFLAG% --target=x86_64-pc-windows-msvc -fuse-ld=lld %LINK_FLAGS% %SAN% %LINK_OBJECTS% "%DIR%\application.res" %NATIVE_LIBS% %LINK_EXTRA% -Xlinker /MACHINE:X64 -Xlinker /INCREMENTAL:NO -Xlinker /WX -Xlinker /MANIFEST:NO -Xlinker /SUBSYSTEM:%~2 %PLPT_EXTRA_LDFLAGS% -o "%BIN%\%~1.exe" || exit /b
if not exist "%BIN%\%~1.exe" (call :problem "linker returned success without executable"& exit /b 1)
exit /b 0

:sanitizer_runtime
if not "%MODE%"=="asan" exit /b 0
call :run "%CXX%" -print-resource-dir || exit /b
set "RESOURCE_DIR="
for /f "usebackq delims=" %%I in ("%CMDLOG%") do set "RESOURCE_DIR=%%I"
set "ASAN_DLL=%RESOURCE_DIR%\lib\windows\clang_rt.asan_dynamic-x86_64.dll"
if not exist "%ASAN_DLL%" (echo SKIP: Windows compiler-rt ASan runtime unavailable. Qualification incomplete.& exit /b 77)
call :run copy /y "%ASAN_DLL%" "%BIN%\" || exit /b
exit /b 0

:build_project
call :stamp
set "START_STAMP=%STAMP%"
rem Remove old outputs before validation, so a failed attempt cannot reuse a prior binary.
call :fresh "%DIR%" || exit /b
set "BIN=%DIR%\bin"
mkdir "%BIN%" "%DIR%\objects"
if not exist "%DIR%\objects" exit /b 1
call :toolchain || exit /b
call :dependencies || exit /b
call :configure_flags || exit /b
call :sanitizer_runtime || exit /b
>"%DIR%\application.rc" echo 1 24 "%ROOT:\=/%/packaging/windows/app.manifest"
call :run "%RCEXE%" /nologo /fo "%DIR%\application.res" "%DIR%\application.rc" || exit /b
if not exist "%DIR%\application.res" (call :problem "resource compiler did not create manifest resource"& exit /b 1)
echo Compilation started: %MODE% - deterministic fresh objects, no cache
set "CORE_OBJECTS="
set "MODEL_OBJECTS="
set "GUI_OBJECTS="
set "UI_INCLUDES="
set "LINK_EXTRA="
for %%S in (%CORE_SOURCES%) do (call :core_one "%%S" || exit /b)
call :cpp app/host_shell/main.cpp || exit /b
set "LINK_OBJECTS=%CORE_OBJECTS% "%OBJECT%""
call :link plpt-monitor CONSOLE || exit /b
if "%CORE_ONLY%"=="0" (call :desktop || exit /b)
if "%WITH_TESTS%"=="1" (call :test_targets || exit /b)
call :stamp
set /a "ELAPSED=STAMP-START_STAMP" >nul
if %ELAPSED% LSS 0 set /a "ELAPSED+=8640000" >nul
echo BUILD SUCCESS mode=%MODE% elapsed_centiseconds=%ELAPSED% (including validation/extraction, not tests)
>>"%LOG%" echo BUILD SUCCESS mode=%MODE% elapsed_centiseconds=%ELAPSED%
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT Windows::BUILD SUCCESS mode=%MODE% elapsed_centiseconds=%ELAPSED% core_only=%CORE_ONLY%
>"%DIR%\BUILD_SUCCEEDED" echo mode=%MODE% compiler=%CXX%
exit /b 0

:desktop
mkdir "%DIR%\vendor\sdl" "%DIR%\vendor\imgui"
call :run tar.exe -xf "%SDL_ARCHIVE%" -C "%DIR%\vendor\sdl" --strip-components=1 || exit /b
call :run tar.exe -xzf "%IMGUI_ARCHIVE%" -C "%DIR%\vendor\imgui" --strip-components=1 || exit /b
set "SDL_ROOT=%DIR%\vendor\sdl"
set "IMGUI_ROOT=%DIR%\vendor\imgui"
for %%F in ("%SDL_ROOT%\include\SDL3\SDL.h" "%SDL_ROOT%\lib\x64\SDL3.lib" "%SDL_ROOT%\lib\x64\SDL3.dll" "%IMGUI_ROOT%\imgui.cpp") do (
  if not exist "%%~F" (call :problem "pinned dependency layout missing file %%~F"& exit /b 1)
)
call :run "%DUMPBIN%" /nologo /headers "%SDL_ROOT%\lib\x64\SDL3.lib" || exit /b
findstr /I /C:"8664 machine" "%CMDLOG%" >nul || (call :problem "SDL3 import library is not x64 Microsoft COFF"& exit /b 1)
call :run "%DUMPBIN%" /nologo /headers "%SDL_ROOT%\lib\x64\SDL3.dll" || exit /b
findstr /I /C:"8664 machine" "%CMDLOG%" >nul || (call :problem "SDL3 DLL is not x64 PE"& exit /b 1)
call :run copy /y "%SDL_ROOT%\lib\x64\SDL3.dll" "%BIN%\SDL3.dll" || exit /b
call :runtime_dependencies || exit /b
for %%S in (%IMGUI_SOURCES%) do (call :gui_one "%%S" || exit /b)
set "UI_INCLUDES=-isystem "%SDL_ROOT%\include" -isystem "%IMGUI_ROOT%" -isystem "%IMGUI_ROOT%\backends""
call :cpp app/desktop_ui/main.cpp || exit /b
set "UI_INCLUDES="
set "LINK_OBJECTS=%CORE_OBJECTS% %GUI_OBJECTS% "%OBJECT%""
set "LINK_EXTRA="%SDL_ROOT%\lib\x64\SDL3.lib""
call :link plpt-desktop WINDOWS || exit /b
set "LINK_EXTRA="
call :run "%DUMPBIN%" /nologo /dependents "%BIN%\plpt-desktop.exe" || exit /b
call :run "%MTEXE%" -nologo -inputresource:"%BIN%\plpt-desktop.exe";#1 -out:"%DIR%\embedded.manifest" || exit /b
findstr /C:"asInvoker" "%DIR%\embedded.manifest" >nul || (call :problem "asInvoker manifest missing"& exit /b 1)
findstr /C:"PerMonitorV2" "%DIR%\embedded.manifest" >nul || (call :problem "DPI manifest missing"& exit /b 1)
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT Windows::PASS x64 COFF SDL3 import/DLL and embedded asInvoker/PerMonitorV2 manifest
exit /b 0

:runtime_dependencies
call :run "%DUMPBIN%" /nologo /dependents "%BIN%\SDL3.dll" || exit /b
copy /y "%CMDLOG%" "%DIR%\SDL3-runtime-dependencies.txt" >nul
rem Official SDL DLL may use its own dynamic CRT. Never assume application's /MT removes it.
findstr /I /C:"VCRUNTIME140" /C:"MSVCP140" "%CMDLOG%" >nul
if not "%errorlevel%"=="0" goto runtime_recorded
set "CRT_DIR=%VCToolsRedistDir%x64\Microsoft.VC143.CRT"
if not exist "%CRT_DIR%\vcruntime140.dll" (call :problem "official SDL DLL requires VC runtime; app-local redistributable not found in %CRT_DIR%"& exit /b 2)
call :run copy /y "%CRT_DIR%\*.dll" "%BIN%\" || exit /b
>>"%DIR%\SDL3-runtime-dependencies.txt" echo App-local CRT copied from %CRT_DIR%. See Microsoft redistribution terms.
:runtime_recorded
call :notice_file "%DIR%\SDL3-runtime-dependencies.txt"
exit /b 0

:test_targets
for %%S in (%MODEL_SOURCES%) do (call :model_one "%%S" || exit /b)
call :cpp tests/unit/test_main.cpp || exit /b
set "LINK_OBJECTS=%CORE_OBJECTS% %MODEL_OBJECTS% "%OBJECT%""
call :link plpt-unit-tests CONSOLE || exit /b
call :cpp tests/integration/platform_tests.cpp || exit /b
set "LINK_OBJECTS=%CORE_OBJECTS% "%OBJECT%""
call :link plpt-platform-tests CONSOLE || exit /b
set "HEADER_COUNT=0"
for %%H in (%HEADERS%) do (call :header_one "%%H" || exit /b)
echo PASS %HEADER_COUNT% independent native headers
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT Windows::PASS %HEADER_COUNT% independent native headers
exit /b 0

:header_one
>"%DIR%\header-probe.cpp" echo #include "%~1"
echo Independent header: %~1
call :run "%CXX%" %VFLAG% %COMMON_FLAGS% %PROFILE_FLAGS% %WARNINGS% -I "%ROOT%" -fsyntax-only "%DIR%\header-probe.cpp" || exit /b
set /a HEADER_COUNT+=1 >nul
exit /b 0

:expect
rem The command is stored in TEST_COMMAND; no shell pipeline around the child.
set "CASE_NAME=%~1"
set "EXPECTED=%~2"
set "PATTERN=%~3"
echo Test started: %CASE_NAME%, expected exit=%EXPECTED%
set "RUN_EXPECTED=%EXPECTED%"
call :run %TEST_COMMAND%
set "TEST_RC=%errorlevel%"
set "RUN_EXPECTED="
copy /y "%CMDLOG%" "%DIR%\%CASE_NAME%.log" >nul
if "%CASE_NAME%"=="ipv6" if "%TEST_RC%"=="77" (
  set /a SKIP+=1 >nul
  echo SKIP ipv6: unavailable. NOT counted as passed.
  if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT Windows::SKIP ipv6 - qualification incomplete
  exit /b 0
)
if not "%TEST_RC%"=="%EXPECTED%" (
  echo FAIL %CASE_NAME% expected=%EXPECTED% actual=%TEST_RC% command=%TEST_COMMAND%
  if "%TEST_RC%"=="0" exit /b 1
  exit /b %TEST_RC%
)
findstr /I /R /C:"%PATTERN%" "%CMDLOG%" >nul || (echo FAIL %CASE_NAME% missing expected output %PATTERN%& exit /b 1)
findstr /I /C:"AddressSanitizer" /C:"LeakSanitizer" /C:"UndefinedBehaviorSanitizer" /C:"runtime error:" "%CMDLOG%" >nul
if "%errorlevel%"=="0" (echo FAIL %CASE_NAME% sanitizer diagnostic, even though exit matched& exit /b 1)
set /a PASS+=1 >nul
echo PASS %CASE_NAME% exit=%TEST_RC%
>>"%LOG%" echo PASS %CASE_NAME% exit=%TEST_RC%
>>"%DIR%\test-summary.txt" echo PASS %CASE_NAME% exit=%TEST_RC% mode=%MODE%
findstr /C:"desktop-runtime:" /C:"cases," "%CMDLOG%" >>"%DIR%\test-summary.txt"
exit /b 0

:tests
>"%DIR%\test-summary.txt" echo Native Windows tests mode=%MODE%
set "PASS=0"
set "SKIP=0"
set "TEST_COMMAND="%BIN%\plpt-unit-tests.exe""
call :expect unit 0 "0 failures" || exit /b
set "TEST_COMMAND="%BIN%\plpt-platform-tests.exe""
call :expect platform 0 "0 failures" || exit /b
set "TEST_COMMAND="%BIN%\plpt-platform-tests.exe" --ipv6"
call :expect ipv6 0 "0 failures" || exit /b
set "TEST_COMMAND="%BIN%\plpt-monitor.exe" --help"
call :expect cli-help 0 "no probes, no server, no sharing" || exit /b
set "TEST_COMMAND="%BIN%\plpt-monitor.exe" --version"
call :expect cli-version 0 "no sharing" || exit /b
set "TEST_COMMAND="%BIN%\plpt-monitor.exe" --not-a-plpt-option"
call :expect cli-unknown 2 "Usage:" || exit /b
set "TEST_COMMAND="%BIN%\plpt-monitor.exe" --version extra"
call :expect cli-extra 2 "Usage:" || exit /b
set "TEST_COMMAND="%BIN%\plpt-monitor.exe" --help --version"
call :expect cli-ambiguous 2 "Usage:" || exit /b
if "%CORE_ONLY%"=="0" (call :desktop_tests || exit /b)
echo TEST SUMMARY passed=%PASS% skipped=%SKIP% mode=%MODE%
>>"%LOG%" echo TEST SUMMARY passed=%PASS% skipped=%SKIP% mode=%MODE%
>>"%DIR%\test-summary.txt" echo TEST SUMMARY passed=%PASS% skipped=%SKIP% mode=%MODE%
call :notice_file "%DIR%\test-summary.txt"
if not "%SKIP%"=="0" exit /b 77
exit /b 0

:desktop_tests
set "SDL_VIDEODRIVER=dummy"
set "SDL_RENDER_DRIVER=software"
set "TEST_COMMAND="%BIN%\plpt-desktop.exe" --smoke-test --screenshot "%DIR%\desktop.bmp""
call :expect ui-smoke 0 "desktop-smoke:" || exit /b
set "TEST_COMMAND="%BIN%\plpt-desktop.exe" --not-a-plpt-option"
call :expect ui-unknown 2 "Usage:" || exit /b
set "TEST_COMMAND="%BIN%\plpt-desktop.exe" --smoke-test --screenshot"
call :expect ui-missing-argument 2 "Usage:" || exit /b
set "TEST_COMMAND="%BIN%\plpt-desktop.exe" --screenshot never-created.bmp"
call :expect ui-capture-requires-smoke 2 "--smoke-test" || exit /b
set "SDL_VIDEODRIVER=plpt-invalid-video-driver"
set "TEST_COMMAND="%BIN%\plpt-desktop.exe" --smoke-test"
call :expect ui-driver-error 1 "SDL_Init:" || exit /b
set "SDL_VIDEODRIVER=dummy"
set "TEST_COMMAND="%BIN%\plpt-desktop.exe" --smoke-test --screenshot "%BIN%\plpt-desktop.exe\not-a-directory.bmp""
call :expect ui-file-error 1 "SDL_IOFromFile:" || exit /b
if "%NATIVE_UI%"=="1" (
  set "SDL_VIDEODRIVER=windows"
  call :native_window_test || exit /b
) else echo NOT RUN: native Windows window launch. Use --native-ui; dummy is not native display qualification.
set "SDL_VIDEODRIVER="
set "SDL_RENDER_DRIVER="
exit /b 0
:native_window_test
set "TEST_COMMAND="%BIN%\plpt-desktop.exe" --smoke-test --screenshot "%DIR%\desktop-native.bmp""
call :expect ui-native-windows 0 "video=windows renderer=software" || exit /b
exit /b 0

:fuzz
set "MODE=asan"
set "CORE_ONLY=1"
set "DIR=%OUT%\tests\fuzz"
call :fresh "%DIR%" || exit /b
set "BIN=%DIR%\bin"
mkdir "%BIN%" "%DIR%\objects" "%DIR%\corpus"
call :toolchain || exit /b
call :configure_flags || exit /b
call :sanitizer_runtime || exit /b
if not exist "%RESOURCE_DIR%\lib\windows\clang_rt.fuzzer-x86_64.lib" (echo SKIP: native LLVM libFuzzer unavailable& exit /b 77)
set "SAN=%SAN% -fsanitize=fuzzer-no-link"
set "UI_INCLUDES="
call :cpp engine/protocol/envelope.cpp || exit /b
set "FUZZ_OBJECTS="%OBJECT%""
call :cpp tests/fuzz/envelope_fuzz.cpp || exit /b
call :run "%CXX%" %VFLAG% --target=x86_64-pc-windows-msvc -fuse-ld=lld %LINK_FLAGS% -fsanitize=address,fuzzer %FUZZ_OBJECTS% "%OBJECT%" -o "%BIN%\plpt-fuzz-envelope.exe" || exit /b
if not exist "%BIN%\plpt-fuzz-envelope.exe" (call :problem "fuzzer output missing"& exit /b 1)
copy /y tests\fuzz\corpus\hello "%DIR%\corpus\hello" >nul || exit /b
copy /y tests\fuzz\corpus\chat "%DIR%\corpus\chat" >nul || exit /b
call :run "%BIN%\plpt-fuzz-envelope.exe" -runs=20000 -seed=20261113 -max_len=4096 -timeout=2 "%DIR%\corpus" || exit /b
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT Windows::PASS native libFuzzer plus ASan, 20000 executions
exit /b 0

:package
call :fresh "%OUT%\packages" || exit /b
if "%CORE_ONLY%"=="1" (call :problem "package requires the full native desktop"& exit /b 2)
set "MODE=release"
set "DIR=%OUT%\release"
set "WITH_TESTS=1"
set "NATIVE_UI=1"
call :build_project || exit /b
call :tests || exit /b
set "STAGE=%OUT%\packages\stage"
call :fresh "%STAGE%" || exit /b
mkdir "%STAGE%\PLPT\bin" "%STAGE%\PLPT\docs" "%STAGE%\PLPT\licenses"
call :run copy /y "%BIN%\plpt-monitor.exe" "%STAGE%\PLPT\bin\" || exit /b
call :run copy /y "%BIN%\plpt-desktop.exe" "%STAGE%\PLPT\bin\" || exit /b
call :run copy /y "%BIN%\*.dll" "%STAGE%\PLPT\bin\" || exit /b
for %%F in (README.md docs\BUILD.md docs\SECURITY.md docs\QUICKSTART.md third_party\DEPENDENCIES.md) do (call :run copy /y "%%F" "%STAGE%\PLPT\docs\" || exit /b)
for %%F in (SDL3.txt ImGui.txt Bundled-notices.txt) do (call :run copy /y "third_party\licenses\%%F" "%STAGE%\PLPT\licenses\" || exit /b)
set "ARCHIVE=%OUT%\packages\PLPT-0.1.0-preview-windows-x64.zip"
call :run tar.exe -a -cf "%ARCHIVE%" -C "%STAGE%" PLPT || exit /b
call :run certutil.exe -hashfile "%ARCHIVE%" SHA256 || exit /b
call :notice_file "%CMDLOG%"
call :fresh "%OUT%\packages\check" || exit /b
call :run tar.exe -xf "%ARCHIVE%" -C "%OUT%\packages\check" || exit /b
for %%F in (plpt-monitor.exe plpt-desktop.exe SDL3.dll) do (call :run fc.exe /b "%BIN%\%%F" "%OUT%\packages\check\PLPT\bin\%%F" || exit /b)
for %%F in (SDL3.txt ImGui.txt Bundled-notices.txt) do (call :run fc.exe /b "third_party\licenses\%%F" "%OUT%\packages\check\PLPT\licenses\%%F" || exit /b)
call :run "%OUT%\packages\check\PLPT\bin\plpt-monitor.exe" --version || exit /b
set "SDL_VIDEODRIVER=windows"
set "SDL_RENDER_DRIVER=software"
call :run "%OUT%\packages\check\PLPT\bin\plpt-desktop.exe" --smoke-test || exit /b
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT Windows::PASS Release ZIP extracted and executed using native Windows video
exit /b 0

:clean_action
for %%D in (debug release tests packages doctor) do (
  call :plain_directory "%OUT%\%%D" || exit /b
  if exist "%OUT%\%%D" rmdir /s /q "%OUT%\%%D"
  if exist "%OUT%\%%D" (call :problem "clean failed for %%D"& exit /b 1)
)
echo CLEAN SUCCESS: compiled outputs removed. Verified archives and logs retained.
exit /b 0

:selftest
rem Child script calls exercise the same authoritative path; no helper build engine.
rmdir "%OUT%\.build-lock"
set "LOCK=0"
set "PLPT_EXPECT_FAILURE=1"
set "SAVE_CXX=%PLPT_CXX%"
set "PLPT_CXX=C:\plpt-missing-compiler\clang++.exe"
call "%ROOT%\build.bat" doctor --core-only >"%OUT%\logs\selftest-missing-compiler.log" 2>&1
set "SELF_RC=%errorlevel%"
set "PLPT_CXX=%SAVE_CXX%"
if "%SELF_RC%"=="0" (call :problem "missing compiler falsely succeeded"& exit /b 1)
set "PLPT_EXTRA_CXXFLAGS=-fplpt-deliberate-invalid-option"
call "%ROOT%\build.bat" debug --core-only >"%OUT%\logs\selftest-compile-failure.log" 2>&1
set "SELF_RC=%errorlevel%"
set "PLPT_EXTRA_CXXFLAGS="
if "%SELF_RC%"=="0" (call :problem "invalid compiler flag falsely succeeded"& exit /b 1)
if exist "%OUT%\debug\BUILD_SUCCEEDED" (call :problem "stale build success after compilation failure"& exit /b 1)
if exist "%OUT%\debug\bin\plpt-monitor.exe" (call :problem "stale executable after compilation failure"& exit /b 1)
set "PLPT_EXTRA_LDFLAGS=-Xlinker /INCLUDE:plpt_intentionally_missing_symbol"
call "%ROOT%\build.bat" debug --core-only >"%OUT%\logs\selftest-link-failure.log" 2>&1
set "SELF_RC=%errorlevel%"
set "PLPT_EXTRA_LDFLAGS="
if "%SELF_RC%"=="0" (call :problem "unresolved link falsely succeeded"& exit /b 1)
if exist "%OUT%\debug\BUILD_SUCCEEDED" (call :problem "stale build success after link failure"& exit /b 1)
if exist "%OUT%\debug\bin\plpt-monitor.exe" (call :problem "stale executable after link failure"& exit /b 1)
set "DIR=%OUT%\tests\negative"
call :fresh "%DIR%" || exit /b
>"%DIR%\bad.zip" echo Deliberate corrupt archive
call :verify "%DIR%\bad.zip" "%SDL_SHA%"
if "%errorlevel%"=="0" (call :problem "corrupt archive was accepted"& exit /b 1)
set "TEST_COMMAND="%OUT%\doctor\probe.exe" --fail"
call :expect intentional-exit 0 "should never pass"
if not "%errorlevel%"=="23" (call :problem "test process exit code 23 was not preserved"& exit /b 1)
echo SELFTEST PASS: compiler, compile, link, hash failures and test exit code 23 propagate; no stale successful output
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT Windows::SELFTEST PASS compiler/compile/link/hash/exit-code propagation
exit /b 0
