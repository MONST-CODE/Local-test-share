# P0 — MSYS2 UCRT64 toolchain correction

Date: 2026-11-13. Owner-mandated Windows toolchain, not an optional profile.

## Read-only reconnaissance — recorded before structural edits

- Inspected the actual checkout, current branch, build scripts, native probe,
  Windows API linkage, SDL/ImGui integration, CI and master §26.
- Baseline: `3b62b48585c2ce095cca4681a2aad4f466fec85f` of main (merged PR #2).
- Dedicated Arena branch already supplied: `arena/4197f0b0-local-test-share`.
  It starts at that baseline; no other branch will be created or switched to.
- Working tree was clean. Existing direct build architecture is retained.
- `build.bat` currently initializes Visual Studio, requires Microsoft SDK/STL,
  targets `x86_64-pc-windows-msvc`, invokes lld-link/SDK resource and inspection
  tools and consumes the VC SDL3 ZIP. **This is the wrong owner toolchain.**
- The native Windows successes in the earlier reports are real but qualify only
  the now-superseded Microsoft profile. They do not qualify MSYS2 UCRT64.
- `build.sh` already directly compiles native Linux clang++/SDL/ImGui and has a
  separate native X11 regression workflow. Its architecture must be preserved.
- UI backend includes currently omit `backends/`; the owner requires the explicit
  `<backends/imgui_impl_sdl3.h>` and `<backends/imgui_impl_sdlrenderer3.h>` paths.
- `tests/build/toolchain_probe.cpp` explicitly rejects MinGW today; only its
  Windows probe must be corrected, not the application core.
- No VS Code configuration exists. Editor tasks must delegate to `build.bat`.
- SDL3 UCRT64 package reference supplied by the owner: version **3.4.20-1**,
  SHA-256 `4a44381560c433ff42da3a6aff788fba2bba84a44b7f63ea396dec9c1cf79f78`.
  Actual package bytes, installed layout, PE imports and runtime behavior still
  require verification on Windows. No successful UCRT64 result is assumed.
- Dear ImGui remains **1.93.0**, commit
  `c1aaac65e896a45f81f47a4d68f9959e0f86c859`, archive SHA-256
  `b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114`.
- This editing environment is Linux, not native Windows. Native Windows Actions
  execution is required; cross-compilation or syntax checking cannot close the gate.

## Implementation plan and proof gates

1. Resolve only `%PLPT_MSYS2_ROOT%\ucrt64\bin\clang++.exe`, default root
   `C:\msys64`. No PATH compiler discovery, PLPT_CXX substitution, CLANG64,
   MSVC, SDK initialization or competing driver.
2. Validate default target, compiler/search paths, MinGW-w64 UCRT headers/imports,
   GNU libstdc++, native x64 C++20/BCrypt/thread execution and GNU linker resolution.
   Preserve diagnostics and fresh output/capture/failure semantics.
3. Use the installed, pinned UCRT64 SDL3 headers/import library/DLL; verify package
   provenance and runtime closure. Compile pinned ImGui core/SDL3/SDLRenderer3
   directly. Support a documented, verified existing ImGui source directory.
4. Use UCRT64 windres/objdump through the script; preserve and check asInvoker and
   PerMonitorV2. Package and inspect all required non-system runtime DLLs and notices.
5. Add VS Code tasks and IntelliSense for the exact owner compiler; verify task
   commands natively. Report automated task-contract validation separately from
   interactive VS Code UI testing.
6. Provision UCRT64 prerequisites in Windows CI, then invoke `build.bat` only for
   project operations. Keep Linux regression coverage. Memory/fuzz support must be
   probed for this exact toolchain; unavailable support is SKIP, never a fake PASS.
7. Execute native doctor, Debug/Release, unit/platform/IPv4/IPv6/error/native-window,
   clean-checkout/rebuild, failure and extracted-package tests. Record versions,
   runtime dependencies, hashes, commands, outcomes and unexecuted boundaries in
   `UCRT64_WINDOWS_VALIDATION.md` and `UCRT64_DEPENDENCIES.md`.
8. Preserve historical evidence with an explicit superseded-profile notice; open
   a new PR from this branch. No merge, main edit, history rewrite or force push.

No C++ core redesign, transport, WebView2, sharing feature or product-architecture
change is authorized by this correction. The master document remains unchanged.

## Implemented changes — what the correction actually does

Every item below is executed by the official scripts, not by a hidden engine.
`build.bat` remains the only Windows orchestrator; `build.sh` is untouched
(SHA-256 `877eb4e2b6c1b30000d87019a71889c7f8a4971f7d16a2c9d0a1e56b0d5973700`).

### 1. Compiler resolution (no discovery, no substitution)

- Root: `PLPT_MSYS2_ROOT`, default `C:\msys64`, validated as an existing
  directory containing `ucrt64\bin\clang++.exe`.
- Compiler: `%PLPT_MSYS2_ROOT%\ucrt64\bin\clang++.exe`, absolute, used for every
  C++ compile and every link. No `where`/PATH lookup, no `PLPT_CXX` override —
  an override attempt is rejected in the selftest.
- PATH for build steps is reduced to `%UCRT%\bin;%SystemRoot%\System32;%SystemRoot%`,
  so MSVC, standalone LLVM, MINGW64 and MSYS `/usr` binaries cannot be selected.
- `MSYSTEM=CLANG64` (or any non-UCRT64 value) is rejected before compilation.

### 2. Doctor: recorded identity and hard rejections

Verified natively on Windows (Actions run for commit `a156a1c`, job
`Native Windows UCRT64 / owner clang++`), from `docs/reports/evidence/ucrt64-*/`:

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

Native probe, executed with the resolved compiler and checked by exit code:

```text
toolchain-probe: selected compiler=C:\msys64\ucrt64\bin\clang++.exe
toolchain-probe: MSYS2 UCRT64, GNU ABI, clang=23.1.5, C++=202002, pointer=8,
                 libstdc++=20260807, GCC=16, _UCRT=1
runtime: ucrtbase.dll=C:\Windows\System32\ucrtbase.dll
runtime: libstdc++-6.dll=C:\msys64\ucrt64\bin\libstdc++-6.dll
runtime: libwinpthread-1.dll=C:\msys64\ucrt64\bin\libwinpthread-1.dll
probe-exit-code=PASS native C++20/x64/thread/BCrypt, no MSVC headers
```

Probe imports (UCRT64 `objdump -p`): `bcrypt.dll`, `libgcc_s_seh-1.dll`,
`KERNEL32.dll`, `api-ms-win-crt-*` UCRT forwarders, `libwinpthread-1.dll`,
`libstdc++-6.dll`. No `msvcrt.dll`, no `VCRUNTIME140.dll`, no `api-ms-win-crt`
MSVC-STL artefact.

The probe additionally proves exact provenance: it requires `PLPT_MSYS2_ROOT`,
canonicalizes `<root>/ucrt64/bin`, and fails unless the compiler that built it is
that directory's `clang++.exe` and unless `libstdc++-6.dll` / `libwinpthread-1.dll`
were loaded from the same UCRT64 `bin`.

Rejections enforced by `build.bat` (each one aborts with a non-zero code):

| Rejected | How it is detected |
| --- | --- |
| `x86_64-pc-windows-msvc` / MSVC ABI | `-dumpmachine` and `--version` must equal the GNU triple; `-v` log scanned for `windows-msvc` |
| MSVC STL | probe refuses `_MSC_VER`/`__has_include(<yvals.h>)`; imports scanned for `msvcrt`/`VCRUNTIME`/`libc++` |
| Microsoft SDK include/lib paths | every resolved include entry must contain `<root>/ucrt64/`; `INCLUDE`/`LIB` are never read |
| MinGW64 (MSVCRT) profile | paths containing `\msys64\mingw64\` are foreign |
| CLANG64 profile | `MSYSTEM` must be `UCRT64`; `\clang64\` paths are foreign |
| MSYS `/usr` as toolchain | `\usr\` and `/usr/` entries rejected in real search paths |
| Standalone LLVM | compiler must be inside `<root>\ucrt64\bin`; `C:\Program Files\LLVM` never probed |
| `clang.exe` / `clang-cl.exe` / `gcc.exe` / `g++.exe` / `cl.exe` | only `%CXX%` (`clang++.exe`) is ever invoked |
| `vswhere`, `VsDevCmd.bat`, `rc.exe`, `mt.exe`, `dumpbin.exe`, `lld-link`, `/MACHINE`, `/INCREMENTAL`, `/MANIFEST`, `-fms-runtime-lib` | removed from the script; UCRT64 `windres.exe`/`objdump.exe`/GNU `ld` used instead |

Legacy GCC probe directories that clang prints as
`ignoring nonexistent directory "…"` are explicitly not treated as real search
paths; only the extracted `#include <...>` list and `-print-search-dirs` entries
that name `<root>/ucrt64/` are checked. That distinction is what made the doctor
both honest and non-flaky.

### 3. Microsoft SDK removal

`WindowsSdkDir`, `WindowsSDKVersion`, `VCToolsInstallDir`, `VSCMD_ARG_TGT_ARCH`,
MSVC `INCLUDE`/`LIB` validation, `vswhere.exe`, `VsDevCmd.bat`, `rc.exe`, `mt.exe`,
`dumpbin.exe`, Microsoft `.lib` inputs, `lld-link`, all MSVC linker switches and
`-fms-runtime-lib` are gone from `build.bat`. Native APIs are linked from UCRT64
GNU imports (`-lws2_32 -liphlpapi -lbcrypt -lkernel32 -luser32 -lgdi32 -lshell32`),
and BCrypt RNG/hash calls are executed by the probe and by the platform tests.

The DPI/execution manifest is preserved: `packaging/windows/app.manifest` is
compiled with UCRT64 `windres.exe`, linked through `clang++`, and the resulting
PE is inspected with UCRT64 `objdump.exe`; the probe also asserts that resource #1
contains `asInvoker` and `PerMonitorV2`.

### 4. Dependencies

SDL3 comes from the installed UCRT64 package only — `ucrt64/include/SDL3`,
`ucrt64/lib/libSDL3.dll.a` (linked), `ucrt64/lib/libSDL3.a` (recorded),
`ucrt64/bin/SDL3.dll` (packaged). The Microsoft VC ZIP and `SDL3.lib` are no
longer referenced anywhere. ImGui 1.93.0 is compiled directly (core + SDL3 +
SDLRenderer3 backends) with the owner-required includes
`<backends/imgui_impl_sdl3.h>` and `<backends/imgui_impl_sdlrenderer3.h>`; an
already-present local source tree is accepted through the documented
`PLPT_IMGUI_DIR` after version/commit/hash verification. No CMake, Ninja, Make,
MSBuild, Meson, Bazel, SCons or CTest is used to build either dependency.

### 5. Editor integration

`.vscode/tasks.json` and `.vscode/c_cpp_properties.json` point IntelliSense at
`C:/msys64/ucrt64/bin/clang++.exe` with `--no-default-config`, C++20 and UCRT64
include discovery, and all tasks delegate to `cmd /d /c build.bat debug|release|test|clean`.
`tests/build/vscode_contract.cpp` parses both JSON files and asserts the contract
natively: `PASS native VS Code task/config contract; interactive editor UI not tested`.
No Visual Studio project generation exists.

### 6. CI

`.github/workflows/build.yml` provisions MSYS2 with the pinned
`msys2/setup-msys2@ec48f7c5447b3140e2b088413ae3a55687bccb6e` action in the UCRT64
profile, installs only the pinned UCRT64 packages, then invokes `build.bat` for
every project operation from ordinary `cmd.exe`. No Visual Studio initialization,
no Windows SDK, no Program Files LLVM. The Linux job is unchanged, so the
correction cannot regress `build.sh`.
