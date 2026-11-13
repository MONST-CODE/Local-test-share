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
