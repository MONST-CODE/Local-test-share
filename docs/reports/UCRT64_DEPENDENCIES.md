# MSYS2 UCRT64 dependency ledger

Date: 2026-11-13. **Native qualification pending** at this initial implementation.
Only `build.bat` defines Windows dependencies/compiler flags and project operations.

## Compiler, headers and libraries

- Default exact compiler: `C:\msys64\ucrt64\bin\clang++.exe`.
- Custom installation: set `PLPT_MSYS2_ROOT`; the only resolution is
  `%PLPT_MSYS2_ROOT%\ucrt64\bin\clang++.exe`. No `PLPT_CXX` or PATH fallback.
- Required default triple: `x86_64-w64-windows-gnu`; native x64 GNU ABI,
  MinGW-w64 **UCRT** headers/import libraries and **GNU libstdc++**.
- `doctor` records actual compiler/package versions, search directories, header
  discovery, selected GNU ld, resource directory, resolved `.a` libraries, PE imports
  and loaded runtime DLL paths. It compiles/links/executes the C++20/thread/BCrypt probe.
- Required native API import archives: `ucrt64/lib/lib{ws2_32,iphlpapi,bcrypt,kernel32,user32,gdi32,shell32}.a`.
- GNU resource/inspection/archive utilities: `ucrt64/bin/{windres,objdump,bsdtar}.exe`.
  Windres uses this same **clang++** as its preprocessor, not GCC/clang.exe.
- No Microsoft SDK, Build Tools, MSVC ABI/STL, standalone LLVM or CLANG64 selection.
  Windows API headers are still valid API usage, supplied by MinGW-w64, not an SDK dependency.

Tool packages are installed by the owner/MSYS2 prerequisite step; their **actual
versions must be recorded**, not represented as immutable compiler pins. CI's
`msys2/setup-msys2` action is pinned to `ec48f7c5447b3140e2b088413ae3a55687bccb6e`.
Upstream package recipes may use other build systems to produce the published
packages; PLPT does **not** run those recipes or build SDL with CMake.

## SDL3 — installed UCRT64 package, never the VC ZIP

| Field | Required identity |
|---|---|
| Package | `mingw-w64-ucrt-x86_64-sdl3` |
| Version | **3.4.20-1** |
| Binary archive | `mingw-w64-ucrt-x86_64-sdl3-3.4.20-1-any.pkg.tar.zst` |
| URL | https://repo.msys2.org/mingw/ucrt64/mingw-w64-ucrt-x86_64-sdl3-3.4.20-1-any.pkg.tar.zst |
| SHA-256 | `4a44381560c433ff42da3a6aff788fba2bba84a44b7f63ea396dec9c1cf79f78` |
| Headers | `%PLPT_MSYS2_ROOT%\ucrt64\include\SDL3\` |
| Selected shared import library | `%PLPT_MSYS2_ROOT%\ucrt64\lib\libSDL3.dll.a` |
| Static archive, verified but not linked | `%PLPT_MSYS2_ROOT%\ucrt64\lib\libSDL3.a` |
| Runtime | `%PLPT_MSYS2_ROOT%\ucrt64\bin\SDL3.dll` |
| License / origin | Zlib; SDL and MSYS2 package maintainers |

The owner-supplied hash is the required digest, **not yet an observed verification**
at this draft. `deps` can reuse the exact package from the existing pacman cache;
`deps --fetch` explicitly permits downloading that same immutable filename plus
pinned ImGui. It never installs/upgrades the owner's packages. Other operations
never download. Every desktop build rechecks archive hashes and byte-compares the
installed SDL headers, both GNU libraries and DLL against a fresh verified extraction.
The DLL/header runtime version is also checked by the application. No `.lib` is used.

## Dear ImGui — existing sources supported

- **1.93.0**, commit `c1aaac65e896a45f81f47a4d68f9959e0f86c859`, MIT.
- Archive https://codeload.github.com/ocornut/imgui/tar.gz/c1aaac65e896a45f81f47a4d68f9959e0f86c859
- SHA-256 `b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114`.
- Default: fresh extraction under each build's `vendor/imgui/`. No precompiled ImGui library.
- Existing copy: set `PLPT_IMGUI_DIR` to the directory containing `imgui.h`, core
  sources and `backends/`. Every used source/header is compared byte-for-byte with
  the verified pin. Owner files are not overwritten. A different version/config
  fails explicitly instead of being silently compiled as the qualified version.
- Compiled directly with the exact UCRT64 clang++: core plus SDL3 platform and
  SDLRenderer3 rendering backends only. Public includes use
  `<backends/imgui_impl_sdl3.h>` and `<backends/imgui_impl_sdlrenderer3.h>`;
  SDL includes stay `<SDL3/...>`.

## Runtime closure and redistribution

`objdump` checks x64 PE/COFF and recursively inspects every non-system imported DLL.
Only DLLs from the selected `ucrt64/bin` whose package owner has the
`mingw-w64-ucrt-x86_64-` prefix can be staged. MSVCRT/MSVC/MSYS/libc++ imports are
rejected. Each copied DLL gets an exact source path, package version and SHA-256
in `runtime-dependencies.txt`; its available package license/provenance files are
copied into `licenses/runtime/`. Unknown non-system dependencies fail, not fall back
to another installation. Extracted packages are executed with a **system-only PATH**.

Expected categories, to be confirmed by actual PE evidence:

- UCRT (`ucrtbase.dll` / Windows API sets) and OS DLLs: Windows-owned, not copied.
- GNU libstdc++ / libgcc: GPL with GCC Runtime Library Exception; package notices
  and source availability/redistribution obligations remain applicable.
- MinGW-w64 libwinpthread: package-supplied permissive notices.
- SDL3: Zlib plus bundled notices; broader compiled surface than Linux's small profile.
- libiconv, if imported by MSYS2 SDL: LGPL; dynamic replacement and corresponding
  source/license obligations must be respected. Do not call it an OS component.
- Compiler-rt for ASan, if available: Apache-2.0 with LLVM exception; tests only,
  never bundled in a Release package.

A precise observed DLL/version/hash list belongs to the native validation report,
not an invented fixed list. No blanket legal clearance or signed installer is
claimed. The PLPT license remains the owner's decision. Existing SDL/ImGui and
bundled upstream notices remain in `third_party/licenses/`.

## Linux and update rules

Linux keeps its native clang++/glibc/libstdc++ and explicit SDL source profile,
source archive hash `e6a9c4e75de8fe719f922dddc208627f77754a502909f457a1355981f20c21e0`.
No Windows package is linked into Linux. Re-run both native platforms after changing
any pin, import surface, compiler/runtime or notice. Preserve error-path, manifest,
IPv4/IPv6, native window, available sanitizers/fuzz, clean/rebuild and extracted
package tests. Rolling OS/package changes require new evidence, not inherited claims.
