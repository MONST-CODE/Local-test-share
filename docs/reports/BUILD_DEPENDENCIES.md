# Direct-build dependency ledger

Date: 2026-11-13. Only `build.bat` / `build.sh` own dependency identities, source
lists and integration flags. No generated dependency project or implicit download.

## Immutable identities and layouts

| Dependency | Archive URL | SHA-256 |
|---|---|---|
| SDL3 3.4.20, official Windows VC x64 development distribution | https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-devel-3.4.20-VC.zip | `1ec3009655ee37b1f73e52b3b120acc5afd8abaf9532f3303b1bf1dfe91b5969` |
| SDL3 3.4.20, source commit `f00dc919ca35b9756316e86c4777392de442f9b2` | https://codeload.github.com/libsdl-org/SDL/tar.gz/f00dc919ca35b9756316e86c4777392de442f9b2 | `e6a9c4e75de8fe719f922dddc208627f77754a502909f457a1355981f20c21e0` |
| Dear ImGui 1.93.0, commit `c1aaac65e896a45f81f47a4d68f9959e0f86c859` | https://codeload.github.com/ocornut/imgui/tar.gz/c1aaac65e896a45f81f47a4d68f9959e0f86c859 | `b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114` |

SDL zlib and ImGui MIT notices are retained under `third_party/licenses/` with
`Bundled-notices.txt`. The latter also retains X11 EDID/xsettings notices and
conservative notices for the broader official Windows SDL binary: HIDAPI 0.14.0
(BSD alternative), Valve OpenVR interfaces (BSD-3-Clause) and pinned Khronos
OpenGL/EGL/Vulkan headers (MIT/Apache-2.0; Vulkan header version 1.3.282).
These are components of the same pinned SDL archive, not newly downloaded
runtime integrations. No OpenVR/HID device library is initialized by PLPT. The product/dependency security surface and update plan are
in `third_party/DEPENDENCIES.md`.

Archive cache: `out/{windows,linux}/dependencies/`. Every build verifies hashes and
extracts from the archive into its **fresh** `vendor/{sdl,imgui}` tree. Modifications
of a prior extraction cannot silently enter the next build. An existing corrupt
archive is rejected rather than silently redownloaded. No dependency is fetched
at application runtime or by debug/release/test/package.

### Windows — native ABI and runtime observed

After stripping the archive root, expected files are `vendor/sdl/include/SDL3/SDL.h`,
`vendor/sdl/lib/x64/SDL3.lib`, `vendor/sdl/lib/x64/SDL3.dll`. `dumpbin /headers`
checks Microsoft COFF/PE x64, `/dependents` records runtime requirements. The DLL
is copied next to the executable; application startup rejects a header/runtime
SDL version mismatch. These checks **passed on native Windows** in run
[43423683934](https://github.com/MONST-CODE/Local-test-share/actions/runs/43423683934),
including Debug/Release/ASan execution and an extracted Release ZIP.

Observed toolchain: **LLVM clang++ / LLD 20.1.8**, Microsoft target
`x86_64-pc-windows-msvc`, **SDK 10.0.26100.0**, **VC Tools 14.44.35207**, probe
`_MSC_VER=1944`, `_MSVC_STL_VERSION=143`, `__cplusplus=202002`. Compiler:
`C:\Program Files\LLVM\bin\clang++.exe`; linker: adjacent `lld-link.exe`.

The actual pinned SDL DLL imports only the following OS libraries:
`SETUPAPI`, `WINMM`, `IMM32`, `VERSION`, `KERNEL32`, `USER32`, `GDI32`,
`ADVAPI32`, `SHELL32`, `ole32`, `OLEAUT32` (all `.dll`). No VCRUNTIME/MSVCP
DLL import was present; the app-local VC-CRT copy branch was **not needed or
exercised** by this pinned archive. This is not a general claim about future
SDL releases or other SDK combinations.

Applications/ImGui use Microsoft static CRT (/MTd Debug, /MT Release). The official
SDL DLL has its own CRT choices; the script records imports and, if necessary,
copies the selected toolset's app-local release redistributable. SDK/STL/CRT licenses
and Microsoft redistribution rights must be audited before distributing a release.
The published ZIP digest came from GitHub release asset 706576117. Native Windows
runner runs starting with 43419873428 **downloaded and verified the actual ZIP bytes**
against that digest (certutil SHA-256); corresponding annotations are retained.
This hash verification alone is not a successful ABI/link/runtime qualification.

### Linux

The official release asset list contains **no Linux binary development distribution**.
`build.sh` directly compiles its pinned SDL C source list into `bin/libSDL3.so.0`
using a literal Linux x86-64/glibc config. Default X11/software + dummy profile;
explicit `--headless` uses only dummy and cannot be packaged. No upstream configure
or other build engine runs. The ImGui core and **only** SDL3/SDLRenderer3 backends
compile as objects linked into the UI. Audio/device/HID/GPU/process/dialog/tray
features are disabled or stubbed in this Linux profile. Official Windows SDL has
a broader compiled surface; PLPT initializes only video and chooses software for tests.

System runtime libraries are not bundled: glibc/libm, libstdc++/libgcc_s, libX11 /
libXext and their OS-managed dependencies. The package is tied to its host ABI,
not advertised as a universal static Linux binary. LGPL/GCC runtime exception and
X11 licenses remain applicable. `ldd` output is recorded by package validation.

## Development tools, not runtime requirements for users

- LLVM clang/clang++, lld-link/ASan/UBSan/libFuzzer: Apache-2.0 WITH LLVM-exception.
- Microsoft C++ Build Tools/SDK: native headers, STL, UCRT/import libraries,
  `rc.exe`, `mt.exe`, `dumpbin.exe`, VsDevCmd environment initialization. No MSBuild.
- Bash/cmd and native tar, curl, hash utilities, copy/remove/compare: no project
  generation. Windows uses certutil SHA-256; Linux uses sha256sum.
- CI display environment: native Windows or Linux X11/Xvfb. Prerequisites are
  installed before calling the scripts; CI does not duplicate target definitions.
- CI actions pinned to checkout `11bd71901bbe5b1630ceea73d27597364c9af683` and
  upload-artifact `ea165f8d65b6e75b540449e92b4886f43607fa02` (MIT).

### Sandbox compiler provenance

Outbound host restrictions prevent using an official LLVM release asset here.
The Linux sandbox compiler was provisioned from npm packages
`llvm-runtime-clang-linux-x64`, `llvm-runtime-resources-linux`,
`llvm-runtime-lld-linux-x64` **22.1.8**, project https://github.com/holepunchto/llvm-runtime .
Each tarball's SHA-512 matched registry metadata using openssl; extraction did not
execute lifecycle scripts. Tool cache: `/home/user/.cache/plpt-direct-tools/`.
The actual version/target/native probe were checked. Exact tarball SHA-512 values
and independent matches are retained in
`evidence/direct-build-2026-11-13/linux-local/tool-provenance.txt`. This secondary distribution
is a supply-chain limitation; it is not substituted for native Microsoft qualification.
No CMake/Ninja/Zig/Python/PowerShell engine is provisioned for this migration.

## Update/regression rule

Change pinned commit/archive/hash/notice together, review upstream changes and
licenses, then re-run clean native Windows/Linux Debug/Release, tests, sanitizer,
fuzzer, UI error paths including the BMP leak regression, package and extracted
runtime checks. Review imported DLLs and transitive notices, especially when the
SDL official binary changes. Never introduce a floating branch or hidden tool.
