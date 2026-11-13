# Direct native compilation — authoritative build interface

**Owner decision: `build.bat` and `build.sh` are the only project build engines.**
They invoke compilers, linkers and native utilities directly. No Jenga workspace,
project generator, CMake, Ninja, Make, MSBuild, CTest, CPack, Python or PowerShell
build orchestration is used. The old preview reports are historical, not current
build instructions.

## Commands

| Operation | Windows | Linux |
|---|---|---|
| Verify/provision exact dependency archives | `build.bat deps --fetch` | `./build.sh deps --fetch` |
| Verify toolchain and dependency hashes | `build.bat doctor --verbose` | `./build.sh doctor --verbose` |
| Fresh Debug build | `build.bat debug` | `./build.sh debug` |
| Fresh Release build | `build.bat release` | `./build.sh release` |
| Compile and run Debug tests | `build.bat test` | `./build.sh test` |
| Compile and run Release tests | `build.bat test release` | `./build.sh test release` |
| Compile and run memory tests | `build.bat test asan` | `./build.sh test asan` |
| Compile and run short fuzz campaign | `build.bat fuzz` | `./build.sh fuzz` |
| Test failure propagation | `build.bat selftest` | `./build.sh selftest` |
| Delete compiled outputs | `build.bat clean` | `./build.sh clean` |
| Fresh Release build, tests, archive and extraction test | `build.bat package` | `./build.sh package --native-ui` |

`--verbose` prints every complete command and the compiler driver's underlying
compiler/linker diagnostics. Every invocation is recorded in a log even without
this flag. No build operation implicitly downloads a dependency: only the explicit
`deps --fetch` operation downloads the immutable URLs and checks their fixed hashes.
A missing/corrupt archive is an error, never a fallback to another version.

## Windows x64 first

Install official LLVM clang++ (18+ with a compatible Microsoft STL; the actual
SDK/STL combination must pass the native doctor), Visual Studio C++ Build Tools
with Windows SDK/UCRT x64, and native `curl.exe`, `tar.exe`, `certutil.exe`.
The script may initialize `VsDevCmd.bat -arch=x64 -host_arch=x64` through vswhere.
This initializes an environment only; it never invokes another build engine.
Its log is separate from compiler captures; every process gets a fresh capture
file so inherited SDK helper handles or old output cannot contaminate validation.

The script selects `clang++.exe`, verifies its version/default Microsoft target,
requires adjacent `lld-link.exe`, validates x64 SDK/STL headers/libraries and
INCLUDE/LIB, then **compiles, links and executes a C++20/SDK/thread probe**. A MinGW
compiler or clang-cl driver is rejected. `PLPT_CXX` can select an explicit clang++.exe.

Project flags include `--target=x86_64-pc-windows-msvc`, `-std=c++20`,
`-Wall -Wextra -Wpedantic -Werror -Wshadow`, Windows 10 API level and Unicode.
Debug uses `-O0 -g -fms-runtime-lib=static_dbg` (/MTd intent); Release uses
`-O2 -DNDEBUG -fms-runtime-lib=static` (/MT intent). Linking is driven directly by
clang++/lld-link with explicit native libraries and subsystem. SDK `rc.exe` embeds
the supplied asInvoker/PerMonitorV2 manifest as a resource; linker auto-manifests
are disabled, and `mt.exe` verifies extraction of the embedded manifest.

SDL3 **3.4.20** uses the official **VC** development ZIP (not the MinGW distribution).
The script verifies SHA-256, extracts fresh, checks the import library/DLL machine
with dumpbin, links `lib/x64/SDL3.lib` and copies `lib/x64/SDL3.dll` next to the UI.
It records the DLL imports. If the official DLL needs the VC runtime, the matching
app-local release CRT is copied from the selected Build Tools redistributable.
Microsoft redistribution terms still apply; /MT in PLPT does not magically make a
third-party DLL self-contained. Dear ImGui core and its SDL3/SDLRenderer3 backends
are compiled directly into objects with the application's CRT settings.

`build.bat test debug --native-ui` additionally creates and closes an actual native
Windows SDL window using the software renderer. The log must identify `video=windows`.
The dummy-driver test alone does not qualify a native Windows desktop.

Windows memory mode uses ASan; Linux uses ASan+UBSan. Missing native compiler-rt /
libFuzzer support reports SKIP/exit 77, not a pass. No Debug/ASan runtime is packaged
as a Release dependency.

## Linux native compilation

Development prerequisites (Ubuntu example):

```sh
sudo apt-get install clang g++ libx11-dev libxext-dev curl tar openssl
./build.sh deps --fetch
./build.sh doctor --verbose
./build.sh debug
./build.sh test debug --native-ui
./build.sh release
./build.sh test asan --native-ui
./build.sh fuzz
./build.sh package --native-ui
```

`g++` supplies libstdc++ headers/libraries on these distributions; it does not
compile PLPT. The compiler is native clang++, not a cross compiler. `PLPT_CXX` and
`PLPT_CC` may select matching LLVM clang++/clang executables. A real compile/link/run
probe verifies C++20 threads and the native standard library. The selected linker
and actual compiler version are logged.

SDL 3.4.20 has **no official Linux x64 binary development asset**. `build.sh` therefore
contains an explicit, audited SDL C source list and literal Linux/glibc configuration.
It directly invokes clang and links `libSDL3.so.0`; it does not call the upstream
build machinery. Default profile: POSIX threads/time/filesystem + **X11/software**
and the dummy driver. Optional audio/devices/GPU/dialog/process backends are disabled
or stubbed. This is the current monitor's profile, not a streaming renderer decision.
The shared library is genuinely the SDL runtime, not an artificial archive of PLPT modules.

On a display-less host, explicitly use `--headless` (dummy-only SDL) for build/test
smoke, or initialize an X11 display with `xvfb-run -a ./build.sh test --native-ui`.
Xvfb initializes a display environment, not a project build engine. The packaged
profile must be X11, never the dummy-only sandbox variant. `--core-only` is an
explicit reduced scope and cannot qualify the full desktop/package.

## Output and reliability policy

```text
out/windows/{debug,release,tests,packages,dependencies,doctor,logs}
out/linux/{debug,release,tests,packages,dependencies,doctor,logs}
```

Each configuration has `objects/`, `bin/`, dependency extractions/configuration
and a `BUILD_SUCCEEDED` marker written only after successful linking. Models are
linked to unit tests, not CLI/UI. There are no PLPT/ImGui static archives created
just to simulate another build system; target object sets are explicit in the scripts.

**Every build is fresh.** The target directory, old binaries and old objects are
removed before tool/dependency validation. This intentionally rebuilds on every
source, public/private header, flag, dependency or toolchain change. Compiler `.d`
files are retained for inspection, not used as an unsafe incremental cache. Build
duration is measured (Linux seconds, Windows centiseconds). Optimization of compile
time is deferred until these measurements justify a separately reviewed strategy.

An exclusive output lock prevents concurrent builds from deleting each other's
outputs. After a forcibly terminated process, check that no build remains before
removing a stale `.build-lock`. Logs and verified archives survive `clean`.
Recursive cleanup is limited to fixed generated directories; reparse/symlink output
paths are refused. Do not make `out` a junction/symlink.

Errors preserve native exit codes and show the failed command. Test codes 1/2 are
only successful when explicitly expected by a negative test; sanitizer diagnostics
still fail such tests. IPv6 unavailable is reported as SKIP and the suite returns
77 (qualification incomplete), never a fabricated pass.

Developer-only `PLPT_EXTRA_CXXFLAGS` / `PLPT_EXTRA_LDFLAGS` permit auditing flag-change
rebuilds and negative tests. They are logged and applied to a clean build; do not
set these for release qualification unless their values are recorded.

## Packaging

`package` always recompiles Release and runs required tests first, then uses native
tar/ZIP utilities. Archives include CLI, desktop, the SDL runtime (and any required
Windows app-local CRT), concise docs and third-party notices. They are extracted,
compared and executed before success is reported. No test/model executable is shipped. A package-specific README links only the
included guides, dependency ledger and notices; full validation logs stay in the
source checkout and platform-specific CI artifacts.

These are **portable development archives**, not signed installers. The old CPack
and unqualified NSIS recipe are retired. Windows installation/signing/SmartScreen
and cross-distribution Linux runtime support remain separate product qualification
questions. This migration does not authorize a new sharing feature.

## Current evidence

See `docs/reports/DIRECT_BUILD_MIGRATION.md`, `BUILD_VALIDATION_WINDOWS.md`,
`BUILD_VALIDATION_LINUX.md`, and `BUILD_DEPENDENCIES.md`. Only recorded native runs
qualify a platform; a syntax-only check or the other OS does not.
