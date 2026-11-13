# Native Windows MSYS2 UCRT64 validation

Date: 2026-11-13. Branch: `arena/4197f0b0-local-test-share`.
Base main: `3b62b48585c2ce095cca4681a2aad4f466fec85f`.

Every result below comes from **real execution of the official `build.bat` on a
native Windows x64 runner using MSYS2 UCRT64 `clang++.exe`**, recorded by the
direct script itself into GitHub Actions check annotations and retained logs.
Nothing is estimated, inherited from the superseded Microsoft profile, or derived
from Linux cross-compilation. Skipped capabilities stay skipped and say why.

## 1. Qualification identity

| Identity | Value |
|---|---|
| Qualified branch head | **`498d18d9855488d903a563438c6798385ab15bb2`** |
| PR | [#3](https://github.com/MONST-CODE/Local-test-share/pull/3), **draft, open, `merged=false`**, 9 commits, 23 files, +1387/−386 |
| Run | [43456570072](https://github.com/MONST-CODE/Local-test-share/actions/runs/43456570072), attempt 1, **completed / success** |
| Windows job / check | **131992341565**, `windows-2022`, **success** (17 steps green, 2 capability-gated steps skipped) |
| Linux job / check | **131992341961**, `ubuntu-24.04`, **success** |
| PR checkout (GitHub test merge) | `e5b0993806b4d7bf6124ca0f7fae7630b3062732`, parents `3b62b48` + `498d18d` |
| Test-merge tree | `21c8096091dfa7676ebab19fb35a5b4930a0d6c3` |
| `git rev-parse 498d18d^{tree}` | `21c8096091dfa7676ebab19fb35a5b4930a0d6c3` — **identical**, so the job compiled exactly this head's tracked inputs and no main drift is hidden in it |

The PR checkout is a GitHub-generated test merge, **not an assistant merge into
main**. Main was never modified; no force push, history rewrite or branch switch
occurred.

Uploaded evidence bundles (metadata from the API; see §12 for the access limit):

| Artifact | ID / bytes | GitHub-reported digest | Expires (UTC) |
|---|---|---|---|
| `ucrt64-windows-evidence` | **14053704998** / **23,030,751** | `sha256:4a91ea17701d24fa5a5e779d6ec57d83e778cce91415a95d7ddad7fc0373109f` | 2026-11-27T16:00:30Z |
| `direct-linux-evidence` | **14055555707** / **1,816,127** | `sha256:23e93335d391b49918dc2b1fd424760cff74f1408755d59b286ff77a85e3c055` | 2026-11-27T16:01:01Z |

## 2. Commands actually executed on native Windows

Ordinary `cmd.exe` on the runner. No Visual Studio Developer Command Prompt, no
`VsDevCmd.bat`, no `vswhere.exe`, no Windows SDK installation step. MSYS2 UCRT64
was provisioned first by the pinned
`msys2/setup-msys2@ec48f7c5447b3140e2b088413ae3a55687bccb6e` action
(`msystem: UCRT64`, `release: false` → `C:\msys64`), then the official script ran:

```bat
call build.bat deps --fetch
call build.bat doctor --verbose
call build.bat ide-check
cmd.exe /d /c build.bat debug
cmd.exe /d /c build.bat test
call build.bat test debug --native-ui
cmd.exe /d /c build.bat release
call build.bat test release --native-ui
call build.bat test asan --native-ui      rem not run: ASan runtime absent (see §10)
call build.bat fuzz                        rem not run: libFuzzer unsupported (see §10)
call build.bat selftest
cmd.exe /d /c build.bat clean
call build.bat package
```

The four `cmd.exe /d /c build.bat …` invocations are byte-identical to the VS Code
task command lines, so the editor contract is exercised by the same commands a
VS Code terminal would run.

## 3. Exact compiler, target and search paths (doctor)

Recorded by `build.bat doctor --verbose` on the runner:

```text
PROFILE=MSYS2 UCRT64 root="C:\msys64"
compiler=C:\msys64\ucrt64\bin\clang++.exe
PATH=C:\msys64\ucrt64\bin;C:\Windows\System32;C:\Windows
compiler-exact=C:\msys64\ucrt64\bin\clang++.exe
clang version 23.1.5 (https://github.com/msys2/MINGW-packages efc96badda99c076509ef01986c673d1cd30f725)
Target: x86_64-w64-windows-gnu
Thread model: posix
InstalledDir: C:/msys64/ucrt64/bin
x86_64-w64-windows-gnu
programs: =C:/msys64/ucrt64/bin
libraries: =C:/msys64/ucrt64/lib/clang/23;C:/msys64/ucrt64/lib/gcc/x86_64-w64-mingw32/16.2.0;C:/msys64/ucrt64/x86_64-w64-mingw32/lib;C:/msys64/ucrt64/x86_64-w64-mingw32/mingw/lib;C:/msys64/ucrt64/lib
default-linker=C:/msys64/ucrt64/bin/ld; explicit-linker=C:\msys64\ucrt64\bin\ld.exe
GNU ld (GNU Binutils) 2.47.20260726
libstdc++.dll.a=C:/msys64/ucrt64/lib/libstdc++.dll.a
libucrt.a=C:/msys64/ucrt64/lib/libucrt.a
libws2_32.a=C:/msys64/ucrt64/lib/libws2_32.a
libiphlpapi.a=C:/msys64/ucrt64/lib/libiphlpapi.a
libbcrypt.a=C:/msys64/ucrt64/lib/libbcrypt.a
libkernel32.a=C:/msys64/ucrt64/lib/libkernel32.a
libuser32.a=C:/msys64/ucrt64/lib/libuser32.a
libgdi32.a=C:/msys64/ucrt64/lib/libgdi32.a
libshell32.a=C:/msys64/ucrt64/lib/libshell32.a
 C:/msys64/ucrt64/include/c++/16.2.0
 C:/msys64/ucrt64/include/c++/16.2.0/x86_64-w64-mingw32
 C:/msys64/ucrt64/include/c++/16.2.0/backward
 C:/msys64/ucrt64/lib/clang/23/include
 C:/msys64/ucrt64/include
mingw-w64-ucrt-x86_64-clang 23.1.5-1
mingw-w64-ucrt-x86_64-headers 14.0.0.r598.ge3d18511f-1
mingw-w64-ucrt-x86_64-crt 14.0.0.r598.ge3d18511f-1
mingw-w64-ucrt-x86_64-libwinpthread 14.0.0.r598.ge3d18511f-1
mingw-w64-ucrt-x86_64-binutils 2.47-4
```

Every include search entry, every library search directory, the resource directory,
the explicit linker (`-fuse-ld=` resolution) and all seven native API import
archives resolve **inside `C:\msys64\ucrt64\`**. The C++ standard library is GNU
libstdc++ 16.2.0 from the same installation. The script rejects any entry that does
not, and rejects `x86_64-pc-windows-msvc`, MSVC STL headers, Microsoft SDK
include/lib directories, MINGW64/MSVCRT, CLANG64, MSYS `/usr` and standalone LLVM
before compiling anything.

## 4. Executed native probe (C++20, x64, GNU ABI, UCRT, BCrypt)

```text
	DLL Name: bcrypt.dll
	DLL Name: libgcc_s_seh-1.dll
	DLL Name: KERNEL32.dll
	DLL Name: api-ms-win-crt-convert-l1-1-0.dll
	DLL Name: api-ms-win-crt-environment-l1-1-0.dll
	DLL Name: api-ms-win-crt-filesystem-l1-1-0.dll
	DLL Name: api-ms-win-crt-heap-l1-1-0.dll
	DLL Name: api-ms-win-crt-locale-l1-1-0.dll
	DLL Name: api-ms-win-crt-math-l1-1-0.dll
	DLL Name: api-ms-win-crt-private-l1-1-0.dll
	DLL Name: api-ms-win-crt-runtime-l1-1-0.dll
	DLL Name: api-ms-win-crt-stdio-l1-1-0.dll
	DLL Name: api-ms-win-crt-string-l1-1-0.dll
	DLL Name: libwinpthread-1.dll
	DLL Name: libstdc++-6.dll
toolchain-probe: selected compiler=C:\msys64\ucrt64\bin\clang++.exe
toolchain-probe: MSYS2 UCRT64, GNU ABI, clang=23.1.5 (https://github.com/msys2/MINGW-packages efc96badda99c076509ef01986c673d1cd30f725), C++=202002, pointer=8, libstdc++=20260807, GCC=16, _UCRT=1
runtime: ucrtbase.dll=C:\Windows\System32\ucrtbase.dll
runtime: libstdc++-6.dll=C:\msys64\ucrt64\bin\libstdc++-6.dll
runtime: libwinpthread-1.dll=C:\msys64\ucrt64\bin\libwinpthread-1.dll
probe-exit-code=PASS native C++20/x64/thread/BCrypt, no MSVC headers
```

The probe is a real x64 PE that compiled, linked, ran and returned 0. It asserts
`__cplusplus == 202002`, `sizeof(void*) == 8`, `_UCRT`, GNU libstdc++ (no MSVC STL),
working `std::thread`, working BCrypt RNG/hash calls, and it re-verifies at run time
that `libstdc++-6.dll` and `libwinpthread-1.dll` were loaded from the canonical
`%PLPT_MSYS2_ROOT%\ucrt64\bin` while `ucrtbase.dll` comes from the OS. It also
refuses to pass unless the compiler that produced it is exactly that directory's
`clang++.exe` (`--expect-compiler`), so a foreign driver cannot fake the result.

## 5. Dependencies verified on the runner

```text
SHA256 4a44381560c433ff42da3a6aff788fba2bba84a44b7f63ea396dec9c1cf79f78 mingw-w64-ucrt-x86_64-sdl3-3.4.20-1-any.pkg.tar.zst
SHA256 b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114 imgui-c1aaac65e896a45f81f47a4d68f9959e0f86c859.tar.gz
PASS installed SDL3 3.4.20-1 headers and both GNU libraries and DLL match verified package bytes
pacman -Q → mingw-w64-ucrt-x86_64-sdl3 3.4.20-1
```

The owner's SDL3 digest matches the downloaded package bytes exactly, and the
installed `ucrt64/include/SDL3/`, `ucrt64/lib/libSDL3.dll.a`, `ucrt64/lib/libSDL3.a`
and `ucrt64/bin/SDL3.dll` were byte-compared against a fresh verified extraction of
that package. Dear ImGui 1.93.0 (commit `c1aaac65e896a45f81f47a4d68f9959e0f86c859`)
was compiled directly from the pinned archive — core plus `imgui_impl_sdl3.cpp` and
`imgui_impl_sdlrenderer3.cpp` — through the same `clang++`, with the owner-required
includes `<backends/imgui_impl_sdl3.h>` and `<backends/imgui_impl_sdlrenderer3.h>`.
No `SDL3.lib`, no VC SDL ZIP, no CMake/Ninja/Make for either dependency.

## 6. Debug and Release builds

| Operation (as CI ran it) | Output directory | Result |
|---|---|---|
| `cmd.exe /d /c build.bat debug` | `out\windows\debug\bin` | `BUILD SUCCESS mode=debug elapsed_centiseconds=4941 core_only=0` (49.41 s) |
| `cmd.exe /d /c build.bat test` | `out\windows\tests\debug\bin` | `BUILD SUCCESS mode=debug elapsed_centiseconds=7831` (78.31 s) + `TEST SUMMARY passed=14 skipped=0 mode=debug` |
| `call build.bat test debug --native-ui` | `out\windows\tests\debug\bin` | `BUILD SUCCESS mode=debug elapsed_centiseconds=7786` (77.86 s) + `TEST SUMMARY passed=15 skipped=0 mode=debug` |
| `cmd.exe /d /c build.bat release` | `out\windows\release\bin` | `BUILD SUCCESS mode=release elapsed_centiseconds=6078` (60.78 s) |
| `call build.bat test release --native-ui` | `out\windows\tests\release\bin` | `BUILD SUCCESS mode=release elapsed_centiseconds=9092` (90.92 s) + `TEST SUMMARY passed=15 skipped=0 mode=release` |
| `call build.bat package` (fresh Release rebuild after `clean`) | `out\windows\release\bin` | `BUILD SUCCESS mode=release elapsed_centiseconds=9032` (90.32 s) + `TEST SUMMARY passed=15 skipped=0 mode=release` |

Each operation deletes its output directory first (`:fresh`), recompiles every
translation unit from source with `-std=c++20 -pthread -D_WIN32_WINNT=0x0A00`,
`-Wall -Wextra -Wpedantic -Werror -Wshadow`, links through the same `clang++`
driver with GNU ld (`-Wl,--no-undefined,--fatal-warnings,--dynamicbase,--nxcompat`)
and refuses to report success from a stale binary. Timings include validation and
dependency extraction, not test execution.

Both binaries are produced from MinGW-w64 UCRT headers/imports with GNU libstdc++;
`plpt-desktop.exe` links `ucrt64/lib/libSDL3.dll.a` and carries the manifest
resource compiled by UCRT64 `windres.exe` (preprocessed by this same `clang++`):

```text
PASS SDL3 GNU import library and embedded asInvoker/PerMonitorV2 manifest
PASS 21 independent native headers
```

The manifest is re-inspected on every linked executable by UCRT64 `objdump.exe`
plus the probe's resource check (`requestedExecutionLevel=asInvoker`,
`PerMonitorV2`), so DPI awareness and execution level were not silently dropped.

## 7. Native runtime results

Debug and Release produce the same outcome (mode string differs):

```text
Native Windows tests mode=release
PASS unit exit=0 mode=release
unit: 26 cases, 12449 checks, 0 failures (0 failed cases)
PASS platform exit=0 mode=release
platform: 4 cases, 58 checks, 0 failures (0 failed cases)
PASS ipv6 exit=0 mode=release
ipv6: 2 cases, 41 checks, 0 failures (0 failed cases)
PASS cli-help exit=0 mode=release
PASS cli-version exit=0 mode=release
PASS cli-unknown exit=2 mode=release
PASS cli-extra exit=2 mode=release
PASS cli-ambiguous exit=2 mode=release
PASS ui-smoke exit=0 mode=release
desktop-runtime: SDL=3004020 video=dummy renderer=software
PASS ui-unknown exit=2 mode=release
PASS ui-missing-argument exit=2 mode=release
PASS ui-capture-requires-smoke exit=2 mode=release
PASS ui-driver-error exit=1 mode=release
PASS ui-file-error exit=1 mode=release
PASS ui-native-windows exit=0 mode=release
desktop-runtime: SDL=3004020 video=windows renderer=software
TEST SUMMARY passed=15 skipped=0 mode=release
```

What this proves natively on Windows with the owner toolchain:

- **Process discovery / platform integration** — `plpt-platform-tests` (4 cases,
  58 checks) exercises the real Windows process/toolhelp, path and BCrypt-backed
  random paths; 0 failures.
- **TCP/UDP IPv4 and IPv6, port reservation, registry, envelope/crypto** —
  `plpt-unit-tests` (26 cases, 12,449 checks) and the dedicated IPv6 executable
  (2 cases, 41 checks) bind ephemeral ports with `bind(0)`/`getsockname` and pass.
- **Runtime identity** — `SDL=3004020` is SDL 3.4.20 reported by the running
  application, i.e. the UCRT64 `SDL3.dll`, not a VC build.
- **Real desktop window** — `ui-native-windows` opened a genuine Win32 window
  (`video=windows`, software renderer on a session-based CI runner), rendered one
  ImGui/SDLRenderer3 frame, captured `desktop-native.bmp` and shut down cleanly
  with exit 0. Dummy-driver runs are recorded separately and never counted as
  native display qualification.
- **Error paths** — unknown/missing/ambiguous CLI arguments exit 2 with usage text;
  invalid video driver exits 1 with `SDL_Init:` diagnostics; unwritable screenshot
  path exits 1 with `SDL_IOFromFile:` diagnostics; `--screenshot` without
  `--smoke-test` exits 2. These are executed failures, not asserted expectations.
- **Skips** — `skipped=0`: no test was converted into a pass and none was hidden.

## 8. Runtime DLL closure, package and execution without MSYS2

`objdump -p` closure for the Release binaries (same shape in Debug):

```text
Native UCRT64 PE dependency closure; source root=C:\msys64\ucrt64
PE=…\out\windows\release\bin\plpt-desktop.exe
	DLL Name: bcrypt.dll            DLL Name: libgcc_s_seh-1.dll
	DLL Name: IPHLPAPI.DLL          DLL Name: KERNEL32.dll
	DLL Name: api-ms-win-crt-*.dll  (convert, environment, filesystem, heap,
	                                 locale, math, private, runtime, stdio,
	                                 string, time, utility)
	DLL Name: libwinpthread-1.dll   DLL Name: SDL3.dll
	DLL Name: SHELL32.dll           DLL Name: libstdc++-6.dll
	DLL Name: USER32.dll            DLL Name: WS2_32.dll
PE=…\out\windows\release\bin\plpt-monitor.exe        (no SDL3; bcrypt, iphlpapi, ws2_32, UCRT, libstdc++, libgcc, libwinpthread)
PE=…\out\windows\release\bin\plpt-platform-tests.exe (same family)
PE=…\out\windows\release\bin\plpt-unit-tests.exe     (same family)
```

Every non-system dependency is a UCRT64 DLL, and each was staged with its exact
package provenance and digest:

| Staged DLL | Package owner reported by pacman | SHA-256 (measured on the runner) |
|---|---|---|
| `libgcc_s_seh-1.dll` | `mingw-w64-ucrt-x86_64-libgcc 16.2.0-6` | `791ebf66e99c7c1b4ed3666468cec45a0172b4c8942ac6c84c3e9cab3e1b5270` |
| `libstdc++-6.dll` | `mingw-w64-ucrt-x86_64-libstdc++ 16.2.0-6` | `4e4822dc4aaa99383f581dae7ab9464ae89346c5ca742f7efbcdebe20b0fe7b5` |
| `libwinpthread-1.dll` | `mingw-w64-ucrt-x86_64-libwinpthread 14.0.0.r598.ge3d18511f-1` | `94a9a4e2fe27b4bc827a4b209e9deb4cc3dc34bc025df06b849144f9d4c588f8` |
| `SDL3.dll` | `mingw-w64-ucrt-x86_64-sdl3 3.4.20-1` | `fcf8a99a019c8d2ed947219e7e768aae1f9cef7219ad93c0ab0007b63db1a6f1` |

`ucrtbase.dll` and the `api-ms-win-crt-*` API sets are Windows-owned and are
**not** copied. No `msvcrt.dll`, `VCRUNTIME140.dll`, `ucrtbase` copy, `libc++` or
MSYS `/usr/bin` DLL appears in any import table — the script rejects those outright.

Package, extraction and execution:

```text
SHA256 hash of D:\a\Local-test-share\Local-test-share\out\windows\packages\PLPT-0.1.0-preview-windows-ucrt64-x64.zip:
8b8618e2a8ab719aa0bcb17821b72cfe024b1743d3370ca462c5b0675d697c00
CertUtil: -hashfile command completed successfully.
PASS UCRT64 Release ZIP extracted and executed using native Windows video with system-only PATH
```

The archive was built with Windows `tar.exe`, extracted, every `.exe`/`.dll`
byte-compared with `fc /b` against the staged originals, then the extracted
`plpt-monitor.exe --version` and `plpt-desktop.exe --smoke-test` were executed with
`PATH=%SystemRoot%\System32;%SystemRoot%` — i.e. with **no MSYS2 directory on
PATH** — and with `SDL_VIDEODRIVER=windows`. Both ran from the bundled DLLs, proving
the package's runtime closure is complete and independent of the toolchain install.
The ZIP also carries `docs/runtime-dependencies.txt`, the UCRT64 dependency ledger,
this validation report and the runtime package license files.

This archive hash is a property of that build (it embeds documentation bytes); it is
not a reproducible-archive claim.

## 9. Failure propagation, wrong-environment and clean rebuilds

```text
SELFTEST PASS missing compiler, CLANG64, compiler override, compile/link/hash failures and test exit 23
```

The selftest really executes the negative cases and checks their exit codes:
missing `PLPT_MSYS2_ROOT`, `MSYSTEM=CLANG64`, an attempted `PLPT_CXX` compiler
override, a deliberately invalid compiler option, a deliberately invalid linker
option, a corrupted dependency archive hash, and a test binary that exits 23.
Each must fail; a pass would fail the selftest. Stale-output protection is verified
by `:fresh` plus the rule that a command capture file may never pre-exist.

`cmd.exe /d /c build.bat clean` removed the generated tree (step green), and the
following `package` step rebuilt Release **from that clean state** (90.32 s) and
re-ran all 15 tests, so the clean-rebuild path is qualified rather than assumed.
The job itself started from a fresh `actions/checkout`, i.e. a clean checkout build.

## 10. Honest skips and remaining limitations

| Item | Status | Reason (measured, not assumed) |
|---|---|---|
| ASan/UBSan memory regressions on Windows | **SKIPPED** (workflow step not run; `build.bat test asan` exits 77) | MSYS2's `mingw-w64-ucrt-x86_64-clang 23.1.5-1` ships **no ASan runtime**: `compiler-rt-runtime-bin: none`, and the resource directory contains only `libclang_rt.builtins-x86_64.a`, `libclang_rt.fuzzer-x86_64.a`, `libclang_rt.fuzzer_interceptors-x86_64.a`, `libclang_rt.fuzzer_no_main-x86_64.a`, `libclang_rt.profile-x86_64.a`, `libclang_rt.profile_rocm-x86_64.a`, `liborc_rt-x86_64.a`. Doctor records `asan-dll=; asan-import-lib=;` and `ASan available=false`. This is **not** a memory-safety pass; Linux ASan/UBSan coverage remains the memory gate (§11). |
| libFuzzer on Windows | **SKIPPED** (`build.bat fuzz` exits 77; step not run) | The capability probe (`tests/build/fuzzer_capability_probe.cpp`, compiled `-fsanitize=fuzzer-no-link`, linked `-fsanitize=fuzzer`, run once) did not produce a running binary, so the qualifying run recorded `fuzzer-link-flags=-fsanitize=fuzzer`, `fuzzer-probe=NOT-RUNNING; fuzz stays SKIP, see the retained command log` and `ASan available=false; libFuzzer available=false`. clang's MinGW toolchain support mask (`clang/lib/Driver/ToolChains/MinGW.cpp`, `getSupportedSanitizers`) enables Address/PointerCompare/PointerSubtract/Vptr but **not** `Fuzzer`, so the shipped `libclang_rt.fuzzer-x86_64.a` is not usable through the owner's `clang++` driver. Using it another way would mean bypassing the mandated driver and is not done. The head following this snapshot retains the compiler's own diagnostic for that probe in the evidence annotation. Linux runs the real 20,000-execution libFuzzer gate. |
| Interactive VS Code editor UI | **NOT TESTED** | `PASS native VS Code task/config contract; interactive editor UI not tested`. The contract test parses `.vscode/tasks.json` and `.vscode/c_cpp_properties.json`, asserts `compilerPath` is `C:/msys64/ucrt64/bin/clang++.exe`, C++20 mode, UCRT64 include discovery, `--no-default-config`, and that all four tasks delegate to `cmd /d /c build.bat …`; the same four command lines were then executed natively. Clicking tasks in the editor, the IntelliSense extension host, and editor-side diagnostics UI are outside automated CI. |
| Real GPU/display, DPI scaling, IME, multi-monitor, per-monitor DPI change | **NOT TESTED** | The runner session provides a software renderer; `video=windows` proves a genuine Win32 window and message loop, not GPU acceleration or DPI behaviour on the owner's hardware. |
| Signed installer / Authenticode | **NOT DONE** | Packages remain unsigned development ZIP archives, as before. |
| Owner-machine specifics | **NOT TESTED** | A custom `PLPT_MSYS2_ROOT` location and a pre-existing local ImGui tree via `PLPT_IMGUI_DIR` are implemented, verified byte-for-byte against the pins and covered by the selftest's failure cases, but CI runs the default `C:\msys64` layout with freshly extracted ImGui. |
| Evidence bundles | **NOT DOWNLOADED HERE** | Artifact/log downloads redirect to storage hosts unreachable from this editing sandbox (`api.github.com` works, blob storage returns EOF). Validation is built from real check annotations, job/run/artifact metadata and the tree-identity comparison in §1. Save the bundles before 2026-11-27 if the full logs are needed. |

Nothing above was converted into a PASS. `TEST SUMMARY … skipped=0` refers to the
executed test binaries; the two capability-gated CI steps were **not run at all**
and are reported as skipped by GitHub itself.

## 11. Linux non-regression (same run)

`Native Linux x64 / direct clang++` (`ubuntu-24.04`) is **success** with
`build.sh` byte-identical to the previously qualified head (SHA-256
`877eb4e2506087459d583b8a7a1e3cb2c2f851c4cb90768eed87b05de2673700` in
`BUILD_INPUTS.sha256`):

```text
dependency SHA256 e6a9c4e75de8fe719f922dddc208627f77754a502909f457a1355981f20c21e0 SDL-f00dc919ca35b9756316e86c4777392de442f9b2.tar.gz
dependency SHA256 b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114 imgui-c1aaac65e896a45f81f47a4d68f9959e0f86c859.tar.gz
PASS 20 independent native headers
BUILD SUCCESS mode=asan elapsed_seconds=103 headless=0 core_only=0   → TEST SUMMARY passed=16 skipped=0 mode=asan
BUILD SUCCESS mode=debug elapsed_seconds=63 / 80                      → TEST SUMMARY passed=16 skipped=0 mode=debug
BUILD SUCCESS mode=release elapsed_seconds=93 / 111                   → TEST SUMMARY passed=16 skipped=0 mode=release
PASS native libFuzzer + ASan/UBSan: 20000 executions
SELFTEST PASS: compiler/compile/link/hash failure propagation
BUILD SUCCESS mode=release elapsed_seconds=112 (package rebuild)      → TEST SUMMARY passed=16 skipped=0
PACKAGE SHA256 683558bd8bc657045f3f96cba3c895cce4fd230a2b0d0b49f9975a5fa6891628 PLPT-0.1.0-preview-linux-x64.tar.gz
PASS native Release package extracted and executed
```

Linux keeps native clang++, glibc/libstdc++, the pinned SDL source profile, ASan/UBSan
and a real 20,000-execution libFuzzer run, including `ui-native-x11` and
`ui-disk-full`. The Windows correction did not regress it.

## 12. Requirement coverage

| Owner requirement (§11) | Result |
|---|---|
| 1. `doctor` confirms the exact compiler path | PASS — `compiler-exact=C:\msys64\ucrt64\bin\clang++.exe` |
| 2. Target triple verified | PASS — `x86_64-w64-windows-gnu` from `-dumpmachine` and `--version` |
| 3. No Microsoft SDK paths in include/link search paths | PASS — all entries inside `C:\msys64\ucrt64\`; foreign-path rejection active |
| 4. C++20 probe compiles, links, executes | PASS — `probe-exit-code=PASS`, `C++=202002`, `pointer=8`, `_UCRT=1` |
| 5. Native process discovery compiles and executes | PASS — `platform: 4 cases, 58 checks, 0 failures` |
| 6. TCP/UDP IPv4 and IPv6 tests | PASS — unit + `ipv6: 2 cases, 41 checks, 0 failures` |
| 7. Port reservation tests | PASS — inside unit/platform suites, `bind(0)`/`getsockname` only |
| 8. SDL3 headers and UCRT64 import library link | PASS — `libSDL3.dll.a`, `PASS SDL3 GNU import library …`, runtime `SDL=3004020` |
| 9. Dear ImGui compiles and links with SDL3 | PASS — core + SDL3 + SDLRenderer3 backends compiled directly |
| 10. Native desktop window opens and shuts down | PASS — `ui-native-windows exit=0`, `video=windows renderer=software` |
| 11. Debug build succeeds | PASS — 49.41 s from a fresh checkout |
| 12. Release build succeeds | PASS — 60.78 s |
| 13. Test execution successful | PASS — `passed=15 skipped=0` in Debug and Release |
| 14. Error-path tests pass | PASS — CLI exit 2 cases, `ui-driver-error`/`ui-file-error` exit 1 |
| 15. Package extraction and execution succeed | PASS — `fc /b` identical, executed with system-only PATH |
| 16. Runtime DLL dependencies resolved | PASS — closure + per-DLL package owner and SHA-256 |
| 17. Clean checkout build succeeds | PASS — clean step, then full Release rebuild and retest |
| 18. VS Code task integration verified | PASS (automated contract + native execution of the four task commands); interactive editor UI NOT TESTED |

Historical Microsoft/MSVC/SDK Windows results remain in this repository as a
**superseded toolchain profile**. They are not UCRT64 evidence and are not counted
anywhere above.

Raw records: `docs/reports/evidence/ucrt64-2026-11-13/` (annotations, run/job/
artifact metadata, PR identity, readable excerpts, build-input digests).
