@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem PLPT authoritative Windows build: MSYS2 UCRT64 clang++.exe ONLY.
setlocal DisableDelayedExpansion
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
if /I "%ACTION%"=="release" set "MODE=release"
if /I "%ACTION%"=="package" set "MODE=release"
if /I "%ACTION%"=="asan" set "MODE=asan"
if /I "%ACTION%"=="fuzz" set "MODE=fuzz"
set "VALID="
for %%A in (debug release test asan fuzz clean package doctor deps selftest ide-check help) do if /I "%ACTION%"=="%%A" set "VALID=1"
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
set "CMDLOG="
set "PLPT_COMMAND_INDEX=0"
>"%LOG%" echo PLPT direct Windows operation=%ACTION% mode=%MODE% started=%DATE% %TIME%
mkdir "%OUT%\.build-lock" >nul 2>nul
if not "%errorlevel%"=="0" (call :problem "another build owns %OUT%\.build-lock. Remove only after verifying no build is running."& exit /b 1)
set "LOCK=1"
set "SDL_SHA=4a44381560c433ff42da3a6aff788fba2bba84a44b7f63ea396dec9c1cf79f78"
set "SDL_PACKAGE=mingw-w64-ucrt-x86_64-sdl3-3.4.20-1-any.pkg.tar.zst"
set "IMGUI_SHA=b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114"
set "SDL_ARCHIVE=%DEPS%\%SDL_PACKAGE%"
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
if /I "%ACTION%"=="ide-check" goto ide_check
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
call :dependencies || exit /b
if "%CORE_ONLY%"=="0" call :prepare_imgui "%OUT%\doctor" || exit /b
exit /b 0

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
rem Each invocation gets a NEW capture file: never read stale output or reuse a
rem file handle inherited by a background child of a native environment initializer.
set /a PLPT_COMMAND_INDEX+=1 >nul
set "CMDLOG=%LOG%.command-%PLPT_COMMAND_INDEX%.txt"
if exist "%CMDLOG%" (call :problem "refusing stale command capture: %CMDLOG%"& exit /b 1)
>"%CMDLOG%" type nul
if not exist "%CMDLOG%" (call :problem "cannot create command capture: %CMDLOG%"& exit /b 1)
>"%CMDLOG%.command" echo %*
type "%CMDLOG%.command" >>"%CMDLOG%"
rem Capture each process without a pipeline masking its status. Print diagnostics afterwards.
>>"%LOG%" echo + %*
if "%VERBOSE%"=="1" echo + %*
rem Group the command so redirection belongs to the complete native invocation.
rem CALL sites must not append conditional operators to this variadic subroutine.
(%*) >"%CMDLOG%" 2>&1
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
if not "%PLPT_EXPECT_FAILURE%"=="1" if defined CMDLOG call :notice_file "%CMDLOG%"
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

:paths
rem Never discover a compiler from PATH; the selected installation is the only authority.
if defined PLPT_CXX (call :problem "PLPT_CXX is not supported on Windows. Set PLPT_MSYS2_ROOT instead."& exit /b 2)
if defined PLPT_EXTRA_CXXFLAGS (call :problem "injected compiler flags are not permitted in the UCRT64 profile"& exit /b 2)
if defined PLPT_EXTRA_LDFLAGS (call :problem "injected linker flags are not permitted in the UCRT64 profile"& exit /b 2)
if defined MSYSTEM if /I not "%MSYSTEM%"=="UCRT64" (call :problem "MSYSTEM must be UCRT64 or unset in an ordinary Windows terminal"& exit /b 2)
for %%E in (CPATH CPLUS_INCLUDE_PATH C_INCLUDE_PATH LIBRARY_PATH COMPILER_PATH GCC_EXEC_PREFIX) do (
  if defined %%E (call :problem "ambient %%E would alter compiler search paths. Clear it first."& exit /b 2)
)
if not defined PLPT_MSYS2_ROOT set "PLPT_MSYS2_ROOT=C:\msys64"
for %%I in ("%PLPT_MSYS2_ROOT%") do set "PLPT_MSYS2_ROOT=%%~fI"
set "UCRT=%PLPT_MSYS2_ROOT%\ucrt64"
set "UCRT_SLASH=%UCRT:\=/%"
set "CXX=%UCRT%\bin\clang++.exe"
set "LD=%UCRT%\bin\ld.exe"
set "WINDRES=%UCRT%\bin\windres.exe"
set "OBJDUMP=%UCRT%\bin\objdump.exe"
set "TAR=%UCRT%\bin\bsdtar.exe"
set "PACMAN=%PLPT_MSYS2_ROOT%\usr\bin\pacman.exe"
set "MSYSTEM=UCRT64"
rem Native utilities and UCRT DLLs only; no Developer Command Prompt or SDK setup.
set "PATH=%UCRT%\bin;%SystemRoot%\System32;%SystemRoot%"
set "INCLUDE="
set "LIB="
set "LIBPATH="
for %%F in ("%CXX%" "%LD%" "%WINDRES%" "%OBJDUMP%" "%TAR%" "%PACMAN%" "%SystemRoot%\System32\curl.exe" "%SystemRoot%\System32\certutil.exe") do (
  if not exist "%%~F" (call :problem "required UCRT64/native tool missing: %%~F. See docs/BUILD.md."& exit /b 2)
)
set "DRIVER=--no-default-config --target=x86_64-w64-windows-gnu"
set "LINK_DRIVER=%DRIVER% --ld-path="%LD%" -pthread"
exit /b 0

:check_search_entry
rem Relative compiler-internal entries are not filesystem selections; every entry
rem that names the selected root must still stay inside the selected root.
set "SEARCH_ENTRY=%~1"
if not defined SEARCH_ENTRY exit /b 0
>"%OUT%\doctor\search-entry.txt" echo(%SEARCH_ENTRY%
findstr /L /I /C:"%UCRT_SLASH%/" "%OUT%\doctor\search-entry.txt" >nul
if errorlevel 1 exit /b 0
call :path_in_ucrt "%SEARCH_ENTRY%"

:path_in_ucrt_path
rem Ignore the official compiler's nonexistent legacy GCC probe directories;
rem every path it actually uses must still reside in the selected UCRT64 root.
echo(%~1| findstr /I /L /C:"ignoring nonexistent directory" >nul && exit /b 0
call :path_in_ucrt "%~1"

:path_in_ucrt
set "CHECK_PATH=%~1"
set "CHECK_PATH=%CHECK_PATH:/=\%"
for %%I in ("%CHECK_PATH%") do set "CHECK_PATH=%%~fI"
set "CHECK_PATH=%CHECK_PATH:\=/%"
echo(%CHECK_PATH%| findstr /B /I /L /C:"%UCRT_SLASH%/" >nul
if errorlevel 1 (call :problem "path escapes selected UCRT64 toolchain: %CHECK_PATH%"& exit /b 2)
exit /b 0

:reject_foreign_paths
findstr /L /I /C:"Windows Kits" /C:"Microsoft Visual Studio" /C:"Program Files/LLVM" /C:"Program Files\LLVM" /C:"/mingw64/" /C:"\mingw64" /C:"/clang64/" /C:"\clang64" /C:"/usr/include" /C:"/usr/lib" /C:"windows-msvc" "%~1" >nul
if not errorlevel 1 (call :problem "foreign toolchain path/target in compiler diagnostics: %~1"& exit /b 2)
exit /b 0

:toolchain
call :paths || exit /b
if not exist "%OUT%\doctor" mkdir "%OUT%\doctor"
set "TOOLCHAIN_INFO=%LOG%.toolchain.txt"
>"%TOOLCHAIN_INFO%" echo PROFILE=MSYS2 UCRT64 root="%PLPT_MSYS2_ROOT%"
>>"%TOOLCHAIN_INFO%" echo compiler=%CXX%
>>"%TOOLCHAIN_INFO%" echo PATH=%PATH%
call :run "%PACMAN%" -Qqo /ucrt64/bin/clang++.exe
if errorlevel 1 exit /b
call :exact_output "%CMDLOG%" "mingw-w64-ucrt-x86_64-clang" || (call :problem "clang++.exe is not owned by the UCRT64 clang package"& exit /b 2)
for %%I in ("%CXX%") do >>"%TOOLCHAIN_INFO%" echo compiler-exact=%%~fI
call :run "%CXX%" --version
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%TOOLCHAIN_INFO%"
findstr /C:"clang version" "%CMDLOG%" >nul || (call :problem "selected executable is not clang++"& exit /b 2)
call :run "%CXX%" -dumpmachine
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%TOOLCHAIN_INFO%"
call :exact_output "%CMDLOG%" "x86_64-w64-windows-gnu" || (call :problem "expected default target x86_64-w64-windows-gnu, no target substitution"& exit /b 2)
call :run "%CXX%" -print-search-dirs
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%TOOLCHAIN_INFO%"
call :reject_foreign_paths "%CMDLOG%" || exit /b
set "SEARCH_DIRS="
for /f "usebackq tokens=1,* delims==" %%A in ("%CMDLOG%") do if "%%A"=="libraries: " set "SEARCH_DIRS=%%B"
if not defined SEARCH_DIRS (call :problem "cannot determine actual compiler library search paths"& exit /b 2)
for %%L in ("%SEARCH_DIRS:;=" "%") do (call :check_search_entry "%%~L" || exit /b)
call :run "%CXX%" -print-prog-name=ld
if errorlevel 1 exit /b
set "RESOLVED_LD="
for /f "usebackq delims=" %%I in ("%CMDLOG%") do set "RESOLVED_LD=%%I"
if not defined RESOLVED_LD (call :problem "compiler did not resolve a linker"& exit /b 2)
call :path_in_ucrt "%RESOLVED_LD%" || exit /b
>>"%TOOLCHAIN_INFO%" echo default-linker=%RESOLVED_LD%; explicit-linker=%LD%
call :run "%LD%" --version
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%TOOLCHAIN_INFO%"
call :run "%CXX%" -print-resource-dir
if errorlevel 1 exit /b
set "RESOURCE_DIR="
for /f "usebackq delims=" %%I in ("%CMDLOG%") do set "RESOURCE_DIR=%%I"
call :path_in_ucrt "%RESOURCE_DIR%" || exit /b
for %%L in (libstdc++.dll.a libucrt.a libws2_32.a libiphlpapi.a libbcrypt.a libkernel32.a libuser32.a libgdi32.a libshell32.a) do (
  call :resolved_library "%%L" || exit /b
)
for %%F in (windows.h _mingw.h stdlib.h winsock2.h iphlpapi.h bcrypt.h) do (
  if not exist "%UCRT%\include\%%F" (call :problem "required MinGW-w64 header missing: %UCRT%\include\%%F"& exit /b 2)
)
call :run "%CXX%" %DRIVER% -std=c++20 -pthread -E -v -x c++ tests/build/toolchain_probe.cpp -o NUL
if errorlevel 1 exit /b
rem The -v log lists unused legacy GCC probe directories; only real search entries are path-checked.
findstr /L /I /C:"windows-msvc" /C:"x86_64-pc-windows-msvc" "%CMDLOG%" >nul && (call :problem "MSVC target appeared in compiler diagnostics"& exit /b 2)
findstr /R /C:"^ [A-Za-z]:" "%CMDLOG%" >"%LOG%.include-paths.txt"
if errorlevel 1 (call :problem "cannot identify actual native header search paths"& exit /b 2)
for /f "usebackq tokens=*" %%I in ("%LOG%.include-paths.txt") do (call :path_in_ucrt_path "%%I" || exit /b)
call :reject_foreign_paths "%LOG%.include-paths.txt" || exit /b
type "%LOG%.include-paths.txt" >>"%TOOLCHAIN_INFO%"
call :run "%PACMAN%" -Q mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-headers mingw-w64-ucrt-x86_64-crt mingw-w64-ucrt-x86_64-libwinpthread mingw-w64-ucrt-x86_64-binutils
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%TOOLCHAIN_INFO%"
call :run "%CXX%" %LINK_DRIVER% -std=c++20 -Wall -Wextra -Wpedantic -Werror tests/build/toolchain_probe.cpp -o "%OUT%\doctor\probe.exe" -lbcrypt
if errorlevel 1 exit /b
if not exist "%OUT%\doctor\probe.exe" (call :problem "native probe output missing"& exit /b 1)
call :pe_header "%OUT%\doctor\probe.exe" || exit /b
call :run "%OBJDUMP%" -p "%OUT%\doctor\probe.exe"
if errorlevel 1 exit /b
findstr /C:"DLL Name:" "%CMDLOG%" >>"%TOOLCHAIN_INFO%"
call :reject_runtime "%CMDLOG%" || exit /b
call :run "%OUT%\doctor\probe.exe" --expect-compiler "%CXX%"
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%TOOLCHAIN_INFO%"
>>"%TOOLCHAIN_INFO%" echo probe-exit-code=PASS native C++20/x64/thread/BCrypt, no MSVC headers
set "ASAN_AVAILABLE=false"
set "FUZZ_AVAILABLE=false"
set "ASAN_DLL="
set "ASAN_LIB="
set "FUZZ_LIB="
>>"%TOOLCHAIN_INFO%" echo compiler-rt-runtime-bin:
if exist "%UCRT%\bin\*clang_rt*" (dir /b "%UCRT%\bin\*clang_rt*" >>"%TOOLCHAIN_INFO%") else >>"%TOOLCHAIN_INFO%" echo none
>>"%TOOLCHAIN_INFO%" echo compiler-rt-resource-libs:
if exist "%RESOURCE_DIR%\lib\windows\*" (dir /b "%RESOURCE_DIR%\lib\windows\*" >>"%TOOLCHAIN_INFO%") else >>"%TOOLCHAIN_INFO%" echo none
for %%F in ("%UCRT%\bin\*asan_dynamic-x86_64*.dll") do set "ASAN_DLL=%%~fF"
for %%F in ("%RESOURCE_DIR%\lib\windows\*asan_dynamic-x86_64*.a") do set "ASAN_LIB=%%~fF"
for %%F in ("%RESOURCE_DIR%\lib\windows\*fuzzer-x86_64*.a") do set "FUZZ_LIB=%%~fF"
if defined ASAN_DLL if defined ASAN_LIB set "ASAN_AVAILABLE=true"
>>"%TOOLCHAIN_INFO%" echo asan-dll=%ASAN_DLL%; asan-import-lib=%ASAN_LIB%; fuzzer-lib=%FUZZ_LIB%
set "FUZZ_LINK_SAN=-fsanitize=fuzzer"
if "%ASAN_AVAILABLE%"=="true" set "FUZZ_LINK_SAN=-fsanitize=address,fuzzer"
>>"%TOOLCHAIN_INFO%" echo fuzzer-link-flags=%FUZZ_LINK_SAN%
rem Archive presence is not capability. Build and execute the real two-step
rem recipe so "available=true" always means a fuzzer binary really ran here.
set "EXPECT_FAILURE_SAVED=%PLPT_EXPECT_FAILURE%"
set "PLPT_EXPECT_FAILURE=1"
if defined FUZZ_LIB call :run "%CXX%" %LINK_DRIVER% -std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer-no-link -c tests/build/fuzzer_capability_probe.cpp -o "%OUT%\doctor\fuzzer-probe.o"
if not exist "%OUT%\doctor\fuzzer-probe.o" set "FUZZ_LIB="
if defined FUZZ_LIB call :run "%CXX%" %LINK_DRIVER% -std=c++20 -O1 -g %FUZZ_LINK_SAN% "%OUT%\doctor\fuzzer-probe.o" -o "%OUT%\doctor\fuzzer-probe.exe"
if not exist "%OUT%\doctor\fuzzer-probe.exe" set "FUZZ_LIB="
if defined FUZZ_LIB call :run "%OUT%\doctor\fuzzer-probe.exe" -runs=1 -seed=1 -max_len=64
if errorlevel 1 set "FUZZ_LIB="
if defined FUZZ_LIB (>>"%TOOLCHAIN_INFO%" echo fuzzer-probe=PASS compiled linked and executed 1 run& type "%CMDLOG%" >>"%TOOLCHAIN_INFO%") else >>"%TOOLCHAIN_INFO%" echo fuzzer-probe=NOT-RUNNING; fuzz stays SKIP, see the retained command log
set "PLPT_EXPECT_FAILURE=%EXPECT_FAILURE_SAVED%"
set "EXPECT_FAILURE_SAVED="
if defined FUZZ_LIB set "FUZZ_AVAILABLE=true"
>>"%TOOLCHAIN_INFO%" echo ASan available=%ASAN_AVAILABLE%; libFuzzer available=%FUZZ_AVAILABLE%
if "%ASAN_AVAILABLE%"=="false" echo SKIP capability: UCRT64 ASan runtime not installed. Not a memory-test PASS.
if "%FUZZ_AVAILABLE%"=="false" echo SKIP capability: native UCRT64 libFuzzer probe did not build, link and run. Not a fuzz-test PASS.
if /I "%ACTION%"=="doctor" if defined GITHUB_OUTPUT (
  >>"%GITHUB_OUTPUT%" echo "asan=%ASAN_AVAILABLE%"
  >>"%GITHUB_OUTPUT%" echo "fuzz=%FUZZ_AVAILABLE%"
)
call :notice_file "%TOOLCHAIN_INFO%"
exit /b 0

:resolved_library
call :run "%CXX%" -print-file-name=%~1
if errorlevel 1 exit /b
set "RESOLVED_LIBRARY="
for /f "usebackq delims=" %%I in ("%CMDLOG%") do set "RESOLVED_LIBRARY=%%I"
if not exist "%RESOLVED_LIBRARY%" (call :problem "UCRT64 import/runtime library not resolved: %~1"& exit /b 2)
call :path_in_ucrt "%RESOLVED_LIBRARY%" || exit /b
>>"%TOOLCHAIN_INFO%" echo %~1=%RESOLVED_LIBRARY%
exit /b 0

:pe_header
call :run "%OBJDUMP%" -f "%~1"
if errorlevel 1 exit /b
findstr /C:"architecture: i386:x86-64" "%CMDLOG%" >nul || (call :problem "not an x64 PE/COFF file: %~1"& exit /b 1)
findstr /C:"file format pe-i386" /C:"file format pei-i386" "%CMDLOG%" >nul
if not errorlevel 1 (call :problem "32-bit member in dependency: %~1"& exit /b 1)
exit /b 0

:reject_runtime
findstr /I /C:"DLL Name: msvcrt.dll" /C:"DLL Name: MSVCP" /C:"DLL Name: VCRUNTIME" /C:"DLL Name: msys-" /C:"DLL Name: libc++" "%~1" >nul
if not errorlevel 1 (call :problem "forbidden MSVCRT/MSVC/MSYS/libc++ runtime import in %~1"& exit /b 2)
exit /b 0

:exact_output
rem FOR /F handles both MSYS LF and native CRLF; FINDSTR /X is not portable between them.
set "OUTPUT_LINES=0"
set "OBSERVED_LINE="
for /f "usebackq delims=" %%I in ("%~1") do (
  set /a OUTPUT_LINES+=1 >nul
  set "OBSERVED_LINE=%%I"
)
if not defined OBSERVED_LINE exit /b 1
if not "%OUTPUT_LINES%"=="1" exit /b 1
if not "%OBSERVED_LINE%"=="%~2" exit /b 1
exit /b 0

:contains_exact_line
set "MATCHED_LINE=0"
for /f "usebackq delims=" %%I in ("%~1") do if "%%I"=="%~2" set "MATCHED_LINE=1"
if not "%MATCHED_LINE%"=="1" exit /b 1
exit /b 0

:verify
if not exist "%~1" (call :problem "missing archive %~1. Run build.bat deps --fetch or provision that exact pinned file."& exit /b 2)
call :run "%SystemRoot%\System32\certutil.exe" -hashfile "%~1" SHA256
if errorlevel 1 exit /b
findstr /I /X /C:"%~2" "%CMDLOG%" >nul
if not "%errorlevel%"=="0" (call :problem "SHA-256 mismatch for %~1. Archive NOT extracted."& exit /b 1)
echo SHA256 OK %~2 %~1
>>"%LOG%" echo SHA256 OK %~2 %~1
rem Hash checks always run/log; only provisioning emits repeated CI metadata.
if "%GITHUB_ACTIONS%"=="true" if /I "%ACTION%"=="deps" if /I not "%~x1"==".part" echo ::notice title=PLPT Windows dependency::SHA256 %~2 %~nx1
exit /b 0

:dependencies
if "%CORE_ONLY%"=="1" exit /b 0
call :verify "%SDL_ARCHIVE%" "%SDL_SHA%" || exit /b
call :verify "%IMGUI_ARCHIVE%" "%IMGUI_SHA%" || exit /b
call :run "%PACMAN%" -Q mingw-w64-ucrt-x86_64-sdl3
if errorlevel 1 exit /b
call :exact_output "%CMDLOG%" "mingw-w64-ucrt-x86_64-sdl3 3.4.20-1" || (call :problem "installed SDL3 must be the pinned UCRT64 package 3.4.20-1"& exit /b 2)
set "SDL_VERIFY=%OUT%\doctor\sdl-package"
call :fresh "%SDL_VERIFY%" || exit /b
call :run "%TAR%" -xf "%SDL_ARCHIVE%" -C "%SDL_VERIFY%"
if errorlevel 1 exit /b
call :contains_exact_line "%SDL_VERIFY%\.PKGINFO" "pkgname = mingw-w64-ucrt-x86_64-sdl3" || (call :problem "wrong SDL3 package identity"& exit /b 2)
call :contains_exact_line "%SDL_VERIFY%\.PKGINFO" "pkgver = 3.4.20-1" || (call :problem "wrong SDL3 package revision"& exit /b 2)
for %%F in (lib\libSDL3.dll.a lib\libSDL3.a bin\SDL3.dll) do (
  call :run fc.exe /b "%SDL_VERIFY%\ucrt64\%%F" "%UCRT%\%%F"
  if errorlevel 1 exit /b
)
if not exist "%UCRT%\include\SDL3\SDL.h" (call :problem "installed SDL3 headers missing"& exit /b 2)
for %%F in ("%SDL_VERIFY%\ucrt64\include\SDL3\*.h") do (
  call :run fc.exe /b "%%~fF" "%UCRT%\include\SDL3\%%~nxF"
  if errorlevel 1 exit /b
)
call :pe_header "%UCRT%\lib\libSDL3.dll.a" || exit /b
call :pe_header "%UCRT%\bin\SDL3.dll" || exit /b
call :run "%OBJDUMP%" -p "%UCRT%\bin\SDL3.dll"
if errorlevel 1 exit /b
call :reject_runtime "%CMDLOG%" || exit /b
if "%GITHUB_ACTIONS%"=="true" if /I "%ACTION%"=="deps" echo ::notice title=PLPT UCRT64 dependencies::PASS installed SDL3 3.4.20-1 headers and both GNU libraries and DLL match verified package bytes
exit /b 0

:dependencies_action
call :paths || exit /b
if not exist "%DEPS%" mkdir "%DEPS%"
if not exist "%SDL_ARCHIVE%" if exist "%PLPT_MSYS2_ROOT%\var\cache\pacman\pkg\%SDL_PACKAGE%" (
  call :run copy /y "%PLPT_MSYS2_ROOT%\var\cache\pacman\pkg\%SDL_PACKAGE%" "%SDL_ARCHIVE%"
  if errorlevel 1 exit /b
)
if "%FETCH%"=="1" (
  call :fetch_one "https://repo.msys2.org/mingw/ucrt64/%SDL_PACKAGE%" "%SDL_ARCHIVE%" "%SDL_SHA%" || exit /b
  call :fetch_one "https://codeload.github.com/ocornut/imgui/tar.gz/c1aaac65e896a45f81f47a4d68f9959e0f86c859" "%IMGUI_ARCHIVE%" "%IMGUI_SHA%" || exit /b
)
call :dependencies || exit /b
call :prepare_imgui "%OUT%\doctor" || exit /b
exit /b 0

:prepare_imgui
set "IMGUI_ROOT=%~1\vendor\imgui"
call :fresh "%IMGUI_ROOT%" || exit /b
call :run "%TAR%" -xzf "%IMGUI_ARCHIVE%" -C "%IMGUI_ROOT%" --strip-components=1
if errorlevel 1 exit /b
set "IMGUI_HEADERS=imgui.h imconfig.h imgui_internal.h imstb_rectpack.h imstb_textedit.h imstb_truetype.h backends/imgui_impl_sdl3.h backends/imgui_impl_sdlrenderer3.h"
for %%F in (%IMGUI_SOURCES% %IMGUI_HEADERS%) do (
  if not exist "%IMGUI_ROOT%\%%F" (call :problem "pinned ImGui input missing: %%F"& exit /b 2)
)
if not defined PLPT_IMGUI_DIR exit /b 0
for %%I in ("%PLPT_IMGUI_DIR%") do set "PLPT_IMGUI_DIR=%%~fI"
rem Existing owner sources are read-only inputs, not overwritten or trusted by version text alone.
for %%F in (%IMGUI_SOURCES% %IMGUI_HEADERS%) do (
  call :run fc.exe /b "%IMGUI_ROOT%\%%F" "%PLPT_IMGUI_DIR%\%%F"
  if errorlevel 1 exit /b
)
set "IMGUI_ROOT=%PLPT_IMGUI_DIR%"
echo Using existing verified ImGui sources: %IMGUI_ROOT%
>>"%LOG%" echo Using existing verified ImGui sources: %IMGUI_ROOT%
exit /b 0

:fetch_one
if exist "%~2" (call :verify "%~2" "%~3"& exit /b)
call :run "%SystemRoot%\System32\curl.exe" --fail --location --show-error --proto "=https" --tlsv1.2 --output "%~2.part" "%~1"
if errorlevel 1 exit /b
call :verify "%~2.part" "%~3" || exit /b
move /y "%~2.part" "%~2" >nul
exit /b %errorlevel%

:stamp
for /f "tokens=1-4 delims=:., " %%a in ("%TIME: =0%") do set /a "STAMP=((1%%a-100)*360000)+((1%%b-100)*6000)+((1%%c-100)*100)+(1%%d-100)" >nul
exit /b 0

:configure_flags
set "PROFILE_FLAGS=-O0 -g"
set "SAN="
if "%MODE%"=="release" set "PROFILE_FLAGS=-O2 -DNDEBUG"
if "%MODE%"=="asan" (
  set "PROFILE_FLAGS=-O1 -g"
  set "SAN=-fsanitize=address -fno-omit-frame-pointer"
)
rem Fuzzing is not conditional on ASan: libFuzzer links and runs on its own.
rem ASan is added back by the fuzz operation when this toolchain really has it.
if "%MODE%"=="fuzz" (
  set "PROFILE_FLAGS=-O1 -g"
  set "SAN=-fno-omit-frame-pointer"
)
set "VFLAG="
if "%VERBOSE%"=="1" set "VFLAG=-v"
set "COMMON_FLAGS=%DRIVER% -std=c++20 -pthread -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00"
set "WARNINGS=-Wall -Wextra -Wpedantic -Werror -Wshadow"
set "NATIVE_LIBS=-lws2_32 -liphlpapi -lbcrypt -lkernel32 -luser32 -lgdi32 -lshell32"
set "NEGATIVE_CXX="
set "NEGATIVE_LINK="
if "%PLPT_EXPECT_FAILURE%"=="1" if "%PLPT_SELFTEST_FAIL%"=="compile" set "NEGATIVE_CXX=-fplpt-deliberate-invalid-option"
if "%PLPT_EXPECT_FAILURE%"=="1" if "%PLPT_SELFTEST_FAIL%"=="link" set "NEGATIVE_LINK=-Wl,--plpt-deliberate-invalid-option"
exit /b 0

:cpp
set "SOURCE=%~1"
set "OBJECT=%DIR%\objects\%SOURCE:/=_%.o"
echo Compilation: %SOURCE%
call :run "%CXX%" %VFLAG% %COMMON_FLAGS% %PROFILE_FLAGS% %SAN% %WARNINGS% %NEGATIVE_CXX% -I "%ROOT%" %UI_INCLUDES% -MMD -MP -MF "%OBJECT%.d" -c "%SOURCE%" -o "%OBJECT%"
if errorlevel 1 exit /b
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
set "GUI_OBJECT=%DIR%\objects\imgui_%GUI_SOURCE:/=_%.o"
echo Compilation ImGui: %GUI_SOURCE%
call :run "%CXX%" %VFLAG% %COMMON_FLAGS% %PROFILE_FLAGS% %SAN% -isystem "%IMGUI_ROOT%" -MMD -MP -MF "%GUI_OBJECT%.d" -c "%IMGUI_ROOT%\%GUI_SOURCE%" -o "%GUI_OBJECT%"
if errorlevel 1 exit /b
if not exist "%GUI_OBJECT%" (call :problem "missing ImGui object %GUI_OBJECT%"& exit /b 1)
set "GUI_OBJECTS=%GUI_OBJECTS% "%GUI_OBJECT%""
exit /b 0

:link
rem All links use the same clang++ driver and GNU ld from this installation.
if not exist "%OUT%\doctor\probe.exe" call :toolchain || exit /b
echo Linking: %~1
set "SUBSYSTEM=-mconsole"
rem Windows-subsystem binaries define UNICODE, so SDL3's header-only entry
rem implementation provides wWinMain(), not ANSI WinMain(). The MinGW CRT
rem startup only calls wWinMain when -municode selects crtexewin_w.o; plain
rem -mwindows selects crtexewin.o and dies with "undefined reference to WinMain".
rem SDL's own SDL_RunApp then rebuilds a real UTF-8 argc/argv from the command
rem line, so argument handling stays identical for the smoke tests.
if /I "%~2"=="WINDOWS" set "SUBSYSTEM=-mwindows -municode"
call :run "%CXX%" %VFLAG% %LINK_DRIVER% %PROFILE_FLAGS% %SAN% %LINK_OBJECTS% "%DIR%\application.o" %NATIVE_LIBS% %LINK_EXTRA% %SUBSYSTEM% -Wl,--no-undefined,--fatal-warnings,--dynamicbase,--nxcompat %NEGATIVE_LINK% -o "%BIN%\%~1.exe"
if errorlevel 1 exit /b
if not exist "%BIN%\%~1.exe" (call :problem "linker returned success without executable"& exit /b 1)
call :run "%OUT%\doctor\probe.exe" --manifest "%BIN%\%~1.exe"
if errorlevel 1 exit /b
exit /b 0

:sanitizer_runtime
if not "%MODE%"=="asan" exit /b 0
if "%ASAN_AVAILABLE%"=="false" (echo SKIP: native UCRT64 ASan runtime unavailable; NOT PASS& exit /b 77)
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
if "%CORE_ONLY%"=="0" call :prepare_imgui "%DIR%" || exit /b
>"%DIR%\application.rc" echo 1 24 "%ROOT:\=/%/packaging/windows/app.manifest"
call :run "%WINDRES%" --target=pe-x86-64 --input-format=rc --output-format=coff --preprocessor="%CXX%" --preprocessor-arg=--no-default-config --preprocessor-arg=-E --preprocessor-arg=-xc --preprocessor-arg=-DRC_INVOKED -i "%DIR%\application.rc" -o "%DIR%\application.o"
if errorlevel 1 exit /b
if not exist "%DIR%\application.o" (call :problem "resource compiler did not create manifest resource"& exit /b 1)
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
call :runtime_dependencies || exit /b
call :stamp
set /a "ELAPSED=STAMP-START_STAMP" >nul
if %ELAPSED% LSS 0 set /a "ELAPSED+=8640000" >nul
echo BUILD SUCCESS mode=%MODE% elapsed_centiseconds=%ELAPSED% (including validation/extraction, not tests)
>>"%LOG%" echo BUILD SUCCESS mode=%MODE% elapsed_centiseconds=%ELAPSED%
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT Windows::BUILD SUCCESS mode=%MODE% elapsed_centiseconds=%ELAPSED% core_only=%CORE_ONLY%
>"%DIR%\BUILD_SUCCEEDED" echo mode=%MODE% compiler=%CXX%
exit /b 0

:desktop
call :run copy /y "%UCRT%\bin\SDL3.dll" "%BIN%\SDL3.dll"
if errorlevel 1 exit /b
for %%S in (%IMGUI_SOURCES%) do (call :gui_one "%%S" || exit /b)
set "UI_INCLUDES=-isystem "%IMGUI_ROOT%" -isystem "%IMGUI_ROOT%\backends""
call :cpp app/desktop_ui/main.cpp || exit /b
set "UI_INCLUDES="
set "LINK_OBJECTS=%CORE_OBJECTS% %GUI_OBJECTS% "%OBJECT%""
set "LINK_EXTRA="%UCRT%\lib\libSDL3.dll.a""
call :link plpt-desktop WINDOWS || exit /b
set "LINK_EXTRA="
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT UCRT64::PASS SDL3 GNU import library and embedded asInvoker/PerMonitorV2 manifest
exit /b 0

:runtime_dependencies
rem Resolve the complete PE import closure, not just SDL's direct DLL list.
set "RUNTIME_REPORT=%DIR%\runtime-dependencies.txt"
>"%RUNTIME_REPORT%" echo Native UCRT64 PE dependency closure; source root=%UCRT%
call :fresh "%DIR%\runtime-seen" || exit /b
call :fresh "%DIR%\runtime-licenses" || exit /b
set "RUNTIME_ROUND=0"
:runtime_round
set /a RUNTIME_ROUND+=1 >nul
if %RUNTIME_ROUND% GTR 32 (call :problem "runtime dependency closure exceeded 32 rounds"& exit /b 1)
set "RUNTIME_NEW=0"
for %%F in ("%BIN%\*.exe" "%BIN%\*.dll") do (
  call :audit_runtime_file "%%~fF" || exit /b
)
if "%RUNTIME_NEW%"=="1" goto runtime_round
call :notice_file "%RUNTIME_REPORT%"
exit /b 0

:audit_runtime_file
set "PE_FILE=%~1"
set "PE_NAME=%~nx1"
if exist "%DIR%\runtime-seen\%PE_NAME%" exit /b 0
call :pe_header "%PE_FILE%" || exit /b
call :run "%OBJDUMP%" -p "%PE_FILE%"
if errorlevel 1 exit /b
call :reject_runtime "%CMDLOG%" || exit /b
findstr /C:"DLL Name:" "%CMDLOG%" >"%DIR%\runtime-seen\%PE_NAME%.imports"
if errorlevel 1 (call :problem "cannot inspect imports for %PE_FILE%"& exit /b 1)
>>"%RUNTIME_REPORT%" echo PE=%PE_FILE%
type "%DIR%\runtime-seen\%PE_NAME%.imports" >>"%RUNTIME_REPORT%"
for /f "usebackq tokens=3" %%D in ("%DIR%\runtime-seen\%PE_NAME%.imports") do (
  call :resolve_runtime_dll "%%D" || exit /b
)
>"%DIR%\runtime-seen\%PE_NAME%" echo checked
exit /b 0

:resolve_runtime_dll
set "DLL_NAME=%~1"
if /I "%DLL_NAME:~0,11%"=="api-ms-win-" exit /b 0
if /I "%DLL_NAME:~0,11%"=="ext-ms-win-" exit /b 0
rem OS libraries are dependencies, never copied from a developer SDK or redistributed here.
for %%S in (KERNEL32.dll KERNELBASE.dll ntdll.dll USER32.dll GDI32.dll ADVAPI32.dll SHELL32.dll ole32.dll OLEAUT32.dll bcrypt.dll bcryptprimitives.dll WS2_32.dll IPHLPAPI.dll ucrtbase.dll IMM32.dll SETUPAPI.dll VERSION.dll WINMM.dll SHLWAPI.dll DWMAPI.dll CFGMGR32.dll COMDLG32.dll COMBASE.dll USERENV.dll SECUR32.dll CRYPT32.dll SHCORE.dll POWRPROF.dll WINSPOOL.DRV) do if /I "%DLL_NAME%"=="%%S" exit /b 0
if exist "%DIR%\runtime-seen\%DLL_NAME%.provenance" exit /b 0
if not exist "%UCRT%\bin\%DLL_NAME%" (call :problem "unresolved non-system DLL: %DLL_NAME%. Only selected UCRT64 bin is permitted."& exit /b 2)
call :run "%PACMAN%" -Qqo /ucrt64/bin/%DLL_NAME%
if errorlevel 1 exit /b
set "DLL_OWNER="
for /f "usebackq delims=" %%P in ("%CMDLOG%") do set "DLL_OWNER=%%P"
echo(%DLL_OWNER%| findstr /B /L /C:"mingw-w64-ucrt-x86_64-" >nul
if errorlevel 1 (call :problem "non-UCRT64 runtime owner for %DLL_NAME%: %DLL_OWNER%"& exit /b 2)
call :run "%PACMAN%" -Q "%DLL_OWNER%"
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%RUNTIME_REPORT%"
call :run "%SystemRoot%\System32\certutil.exe" -hashfile "%UCRT%\bin\%DLL_NAME%" SHA256
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%RUNTIME_REPORT%"
call :run copy /y "%UCRT%\bin\%DLL_NAME%" "%BIN%\%DLL_NAME%"
if errorlevel 1 exit /b
call :run fc.exe /b "%UCRT%\bin\%DLL_NAME%" "%BIN%\%DLL_NAME%"
if errorlevel 1 exit /b
call :runtime_license "%DLL_OWNER%" || exit /b
>"%DIR%\runtime-seen\%DLL_NAME%.provenance" echo %DLL_OWNER%
set "RUNTIME_NEW=1"
exit /b 0

:runtime_license
set "LICENSE_OWNER=%~1"
if exist "%DIR%\runtime-licenses\%LICENSE_OWNER%\PROVENANCE.txt" exit /b 0
mkdir "%DIR%\runtime-licenses\%LICENSE_OWNER%"
call :run "%PACMAN%" -Qi "%LICENSE_OWNER%"
if errorlevel 1 exit /b
copy /y "%CMDLOG%" "%DIR%\runtime-licenses\%LICENSE_OWNER%\PROVENANCE.txt" >nul || exit /b
call :run "%PACMAN%" -Ql "%LICENSE_OWNER%"
if errorlevel 1 exit /b
findstr /C:"/share/licenses/" "%CMDLOG%" >"%DIR%\runtime-seen\%LICENSE_OWNER%.licenses"
if errorlevel 1 (call :problem "runtime package has no recorded license files: %LICENSE_OWNER%"& exit /b 2)
set "LICENSE_COUNT=0"
for /f "usebackq tokens=2" %%F in ("%DIR%\runtime-seen\%LICENSE_OWNER%.licenses") do (
  call :copy_runtime_license "%%F" || exit /b
)
if "%LICENSE_COUNT%"=="0" (call :problem "no runtime notice copied for %LICENSE_OWNER%"& exit /b 2)
exit /b 0

:copy_runtime_license
set "LICENSE_SOURCE=%~1"
set "LICENSE_SOURCE=%PLPT_MSYS2_ROOT%%LICENSE_SOURCE:/=\%"
if exist "%LICENSE_SOURCE%\" exit /b 0
if not exist "%LICENSE_SOURCE%" (call :problem "runtime notice missing: %LICENSE_SOURCE%"& exit /b 2)
for %%F in ("%LICENSE_SOURCE%") do set "LICENSE_FILE=%%~nxF"
call :run copy /y "%LICENSE_SOURCE%" "%DIR%\runtime-licenses\%LICENSE_OWNER%\%LICENSE_FILE%"
if errorlevel 1 exit /b
set /a LICENSE_COUNT+=1 >nul
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
call :run "%CXX%" %VFLAG% %COMMON_FLAGS% %PROFILE_FLAGS% %WARNINGS% -I "%ROOT%" -fsyntax-only "%DIR%\header-probe.cpp"
if errorlevel 1 exit /b
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
set "MODE=fuzz"
set "CORE_ONLY=1"
set "DIR=%OUT%\tests\fuzz"
call :fresh "%DIR%" || exit /b
set "BIN=%DIR%\bin"
mkdir "%BIN%" "%DIR%\objects" "%DIR%\corpus"
call :toolchain || exit /b
call :configure_flags || exit /b
call :sanitizer_runtime || exit /b
if "%FUZZ_AVAILABLE%"=="false" (echo SKIP: native UCRT64 libFuzzer probe did not run; NOT PASS& exit /b 77)
if "%ASAN_AVAILABLE%"=="true" (set "SAN=%SAN% -fsanitize=address") else echo NOTE: this UCRT64 clang++ has no ASan runtime; fuzzing runs without address instrumentation, never reported as a memory-test PASS.
set "SAN=%SAN% -fsanitize=fuzzer-no-link"
set "UI_INCLUDES="
call :cpp engine/protocol/envelope.cpp || exit /b
set "FUZZ_OBJECTS="%OBJECT%""
call :cpp tests/fuzz/envelope_fuzz.cpp || exit /b
call :run "%CXX%" %VFLAG% %LINK_DRIVER% %PROFILE_FLAGS% %FUZZ_LINK_SAN% %FUZZ_OBJECTS% "%OBJECT%" -o "%BIN%\plpt-fuzz-envelope.exe"
if errorlevel 1 exit /b
if not exist "%BIN%\plpt-fuzz-envelope.exe" (call :problem "fuzzer output missing"& exit /b 1)
call :runtime_dependencies || exit /b
copy /y tests\fuzz\corpus\hello "%DIR%\corpus\hello" >nul || exit /b
copy /y tests\fuzz\corpus\chat "%DIR%\corpus\chat" >nul || exit /b
call :run "%BIN%\plpt-fuzz-envelope.exe" -runs=20000 -seed=20261113 -max_len=4096 -timeout=2 "%DIR%\corpus"
if errorlevel 1 exit /b
set "FUZZ_NOTICE=PASS native libFuzzer without ASan (not installed in this UCRT64 clang++), 20000 executions"
if "%ASAN_AVAILABLE%"=="true" set "FUZZ_NOTICE=PASS native libFuzzer plus ASan, 20000 executions"
>>"%DIR%\fuzz-summary.txt" echo %FUZZ_NOTICE%
>>"%DIR%\fuzz-summary.txt" echo link-flags=%FUZZ_LINK_SAN% compile-flags=%SAN%
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT Windows::%FUZZ_NOTICE%
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
mkdir "%STAGE%\PLPT\bin" "%STAGE%\PLPT\docs\reports" "%STAGE%\PLPT\licenses"
call :run copy /y packaging\README.md "%STAGE%\PLPT\README.md"
if errorlevel 1 exit /b
call :run copy /y docs\reports\BUILD_DEPENDENCIES.md "%STAGE%\PLPT\docs\reports\"
if errorlevel 1 exit /b
call :run copy /y docs\reports\UCRT64_WINDOWS_VALIDATION.md "%STAGE%\PLPT\docs\reports\"
if errorlevel 1 exit /b
call :run copy /y "%BIN%\plpt-monitor.exe" "%STAGE%\PLPT\bin\"
if errorlevel 1 exit /b
call :run copy /y "%BIN%\plpt-desktop.exe" "%STAGE%\PLPT\bin\"
if errorlevel 1 exit /b
call :run copy /y "%BIN%\*.dll" "%STAGE%\PLPT\bin\"
if errorlevel 1 exit /b
for %%F in (docs\BUILD.md docs\SECURITY.md docs\QUICKSTART.md third_party\DEPENDENCIES.md) do (
  call :run copy /y "%%F" "%STAGE%\PLPT\docs\"
  if errorlevel 1 exit /b
)
for %%F in (SDL3.txt ImGui.txt Bundled-notices.txt) do (
  call :run copy /y "third_party\licenses\%%F" "%STAGE%\PLPT\licenses\"
  if errorlevel 1 exit /b
)
call :run copy /y docs\reports\UCRT64_DEPENDENCIES.md "%STAGE%\PLPT\docs\reports\"
if errorlevel 1 exit /b
call :run copy /y "%RUNTIME_REPORT%" "%STAGE%\PLPT\docs\runtime-dependencies.txt"
if errorlevel 1 exit /b
call :run xcopy.exe /E /I /Y "%DIR%\runtime-licenses" "%STAGE%\PLPT\licenses\runtime"
if errorlevel 1 exit /b
set "ARCHIVE=%OUT%\packages\PLPT-0.1.0-preview-windows-ucrt64-x64.zip"
call :run "%TAR%" -a -cf "%ARCHIVE%" -C "%STAGE%" PLPT
if errorlevel 1 exit /b
call :run "%SystemRoot%\System32\certutil.exe" -hashfile "%ARCHIVE%" SHA256
if errorlevel 1 exit /b
call :notice_file "%CMDLOG%"
call :fresh "%OUT%\packages\check" || exit /b
call :run "%TAR%" -xf "%ARCHIVE%" -C "%OUT%\packages\check"
if errorlevel 1 exit /b
for %%F in ("%STAGE%\PLPT\bin\*.exe" "%STAGE%\PLPT\bin\*.dll") do (
  call :run fc.exe /b "%%~fF" "%OUT%\packages\check\PLPT\bin\%%~nxF"
  if errorlevel 1 exit /b
)
call :run fc.exe /b packaging\README.md "%OUT%\packages\check\PLPT\README.md"
if errorlevel 1 exit /b
for %%F in (BUILD_DEPENDENCIES.md UCRT64_WINDOWS_VALIDATION.md) do (
  call :run fc.exe /b "docs\reports\%%F" "%OUT%\packages\check\PLPT\docs\reports\%%F"
  if errorlevel 1 exit /b
)
for %%F in (SDL3.txt ImGui.txt Bundled-notices.txt) do (
  call :run fc.exe /b "third_party\licenses\%%F" "%OUT%\packages\check\PLPT\licenses\%%F"
  if errorlevel 1 exit /b
)
rem Prove the extracted archive does not resolve missing DLLs from the MSYS2 PATH.
set "PATH=%SystemRoot%\System32;%SystemRoot%"
call :run "%OUT%\packages\check\PLPT\bin\plpt-monitor.exe" --version
if errorlevel 1 exit /b
set "SDL_VIDEODRIVER=windows"
set "SDL_RENDER_DRIVER=software"
call :run "%OUT%\packages\check\PLPT\bin\plpt-desktop.exe" --smoke-test
if errorlevel 1 exit /b
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT Windows::PASS UCRT64 Release ZIP extracted and executed using native Windows video with system-only PATH
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
call :paths || exit /b
rem Negative child calls delegate to this script; no alternative compiler is invoked.
rmdir "%OUT%\.build-lock"
set "LOCK=0"
set "PLPT_EXPECT_FAILURE=1"
set "SAVE_ROOT=%PLPT_MSYS2_ROOT%"
set "PLPT_MSYS2_ROOT=C:\plpt-missing-msys2"
call "%ROOT%\build.bat" doctor --core-only >"%OUT%\logs\selftest-missing-compiler.log" 2>&1
set "SELF_RC=%errorlevel%"
set "PLPT_MSYS2_ROOT=%SAVE_ROOT%"
if "%SELF_RC%"=="0" (call :problem "missing UCRT64 compiler falsely succeeded"& exit /b 1)
set "MSYSTEM=CLANG64"
call "%ROOT%\build.bat" doctor --core-only >"%OUT%\logs\selftest-wrong-environment.log" 2>&1
set "SELF_RC=%errorlevel%"
set "MSYSTEM=UCRT64"
if "%SELF_RC%"=="0" (call :problem "CLANG64 was incorrectly accepted"& exit /b 1)
set "PLPT_CXX=C:\Program Files\LLVM\bin\clang++.exe"
call "%ROOT%\build.bat" doctor --core-only >"%OUT%\logs\selftest-compiler-override.log" 2>&1
set "SELF_RC=%errorlevel%"
set "PLPT_CXX="
if "%SELF_RC%"=="0" (call :problem "alternative compiler override was accepted"& exit /b 1)
set "PLPT_SELFTEST_FAIL=compile"
call "%ROOT%\build.bat" debug --core-only >"%OUT%\logs\selftest-compile-failure.log" 2>&1
set "SELF_RC=%errorlevel%"
set "PLPT_SELFTEST_FAIL="
if "%SELF_RC%"=="0" (call :problem "invalid compiler flag falsely succeeded"& exit /b 1)
if exist "%OUT%\debug\BUILD_SUCCEEDED" (call :problem "stale build success after compilation failure"& exit /b 1)
if exist "%OUT%\debug\bin\plpt-monitor.exe" (call :problem "stale executable after compilation failure"& exit /b 1)
set "PLPT_SELFTEST_FAIL=link"
call "%ROOT%\build.bat" debug --core-only >"%OUT%\logs\selftest-link-failure.log" 2>&1
set "SELF_RC=%errorlevel%"
set "PLPT_SELFTEST_FAIL="
if "%SELF_RC%"=="0" (call :problem "invalid linker flag falsely succeeded"& exit /b 1)
if exist "%OUT%\debug\BUILD_SUCCEEDED" (call :problem "stale build success after link failure"& exit /b 1)
if exist "%OUT%\debug\bin\plpt-monitor.exe" (call :problem "stale executable after link failure"& exit /b 1)
set "DIR=%OUT%\tests\negative"
call :fresh "%DIR%" || exit /b
>"%DIR%\bad.pkg" echo Deliberate corrupt archive
call :verify "%DIR%\bad.pkg" "%SDL_SHA%"
if "%errorlevel%"=="0" (call :problem "corrupt archive was accepted"& exit /b 1)
set "TEST_COMMAND="%OUT%\doctor\probe.exe" --fail"
call :expect intentional-exit 0 "should never pass"
if not "%errorlevel%"=="23" (call :problem "test process exit code 23 was not preserved"& exit /b 1)
echo SELFTEST PASS: missing/wrong toolchain, compile, link, hash and test exit 23; no stale successful output
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT UCRT64::SELFTEST PASS missing compiler, CLANG64, compiler override, compile/link/hash failures and test exit 23
exit /b 0

:ide_check
set "DIR=%OUT%\tests\ide"
call :fresh "%DIR%" || exit /b
set "BIN=%DIR%\bin"
mkdir "%BIN%" "%DIR%\objects"
call :toolchain || exit /b
call :configure_flags || exit /b
call :cpp tests/build/vscode_contract.cpp || exit /b
call :run "%CXX%" %LINK_DRIVER% "%OBJECT%" -o "%BIN%\vscode-contract.exe"
if errorlevel 1 exit /b
call :runtime_dependencies || exit /b
call :run "%BIN%\vscode-contract.exe"
if errorlevel 1 exit /b
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=PLPT UCRT64::PASS native VS Code task/config contract; interactive editor UI not tested
exit /b 0
