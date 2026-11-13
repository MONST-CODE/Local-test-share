# Native Windows x64 — direct-build qualification

Date: **2026-11-13**. Status: **PASS for the build-migration gate**.
This is native Windows execution, not a Linux cross-compile or syntax-only check.

## Evidence identity

- Branch: `arena/da48a9c5-local-test-share`, base main `73162d9b857abd0841bdbb116b49e94fb98cdca6`.
- Qualified code/scripts: **`13fd4642f629bc2ca6182e663fa487ee1d9da113`**.
- [Run 43426058514](https://github.com/MONST-CODE/Local-test-share/actions/runs/43426058514): **both native jobs succeeded**.
- Job **131887629384**, runner GitHub-hosted **windows-2022**, native Windows x64.
  The ordinary PR merge
  checkout's tree `e9ca484b59c4a3f04aa7d2c62bdae1f282c29945` was checked against the
  branch commit's tree: identical. See `qualification-pr-source.json` and
  `qualification-merge-tree.json` in the evidence directory.
- [Readable native excerpts](evidence/direct-build-2026-11-13/windows-actions/qualification-excerpts.log)
  and [raw annotations](evidence/direct-build-2026-11-13/windows-actions/qualification-annotations.json).
- [Exact job/step results](evidence/direct-build-2026-11-13/qualification-jobs.json),
  [run metadata](evidence/direct-build-2026-11-13/qualification-run.json),
  [artifact metadata](evidence/direct-build-2026-11-13/qualification-artifacts.json).

The first complete Windows/Linux pass was also observed at `bf89e8e` in run
[43423683934](https://github.com/MONST-CODE/Local-test-share/actions/runs/43423683934).
The later run above re-executed the refined logging, documentation packaging and
notice handling. Subsequent documentation/evidence edits do not change C++ sources
or build scripts. The package hash below belongs to **13fd464**, not to a later
archive containing revised documentation.

## Actual toolchain and ABI

| Item | Observed value |
|---|---|
| Compiler | **LLVM clang++ 20.1.8**, `C:\Program Files\LLVM\bin\clang++.exe` |
| Default/explicit target | `x86_64-pc-windows-msvc` |
| Linker | **LLD 20.1.8**, adjacent `lld-link.exe` |
| Windows SDK | **10.0.26100.0** |
| VC Tools | **14.44.35207** |
| Native probe | `_MSC_VER=1944`, `_MSVC_STL_VERSION=143`, C++ `202002`, x64 |
| Standard library/runtime | Microsoft STL; application `/MTd` intent in Debug, `/MT` in Release |
| Manifest | Extracted resource #1 confirms **asInvoker** and **PerMonitorV2** |

The doctor validates executable/name/version/target, x64 SDK/STL headers and
libraries, INCLUDE/LIB, linker and SDK utilities. It then directly compiles,
links and **runs** `tests/build/toolchain_probe.cpp` (C++20 thread + BCrypt). No
MinGW/GCC fallback or substituted clang-cl driver. Native core/platform tests and
full applications subsequently link with explicit ws2_32/iphlpapi/bcrypt libraries.

SDL3 **3.4.20** uses the official VC archive, freshly extracted after SHA-256
verification. Dumpbin verified **x64 COFF import library and PE DLL**. The runtime
is copied beside the UI; runtime/header version check reports **3004020**. ImGui
**1.93.0** core + SDL3/SDLRenderer3 backends compile directly, not through a library
build generator. Exact archive hashes/layouts are in [BUILD_DEPENDENCIES](BUILD_DEPENDENCIES.md).

## Commands actually executed

All project orchestration is in `build.bat`; the workflow uses `shell: cmd`.

```bat
call build.bat deps --fetch
call build.bat doctor --verbose
call build.bat debug --verbose
call build.bat test debug --native-ui
call build.bat release --verbose
call build.bat test release --native-ui
call build.bat test asan --native-ui
call build.bat fuzz
call build.bat selftest
call build.bat clean
call build.bat package
```

Every required step succeeded, including the build from the fresh checkout and
the package's complete Release rebuild after `clean`. No required step was skipped.

## Measured builds and tests

| Invocation | Clean compilation time | Actual result |
|---|---:|---|
| `debug --verbose` | **26.41 s** | CLI + SDL/ImGui desktop linked |
| `test debug --native-ui` | **42.76 s** | **15 passed, 0 skipped** |
| `release --verbose` | **28.82 s** | CLI + SDL/ImGui desktop linked |
| `test release --native-ui` | **49.73 s** | **15 passed, 0 skipped** |
| `test asan --native-ui` | **69.14 s** | **15 passed, 0 skipped**, native ASan |
| Release rebuild inside `package`, after `clean` | **51.47 s** | **15 passed, 0 skipped**, archive verified and executed |

These are recorded centisecond counters divided by 100, not estimates. Each
measurement includes validation, extraction, fresh compilation/linking and applicable
header checks, but excludes test execution. Hosted-runner variability is expected;
these numbers are not comparative CPU/GPU or LAN performance claims.

Per tested configuration:

- **Unit: 26 cases, 12,449 checks, 0 failures**.
- **Platform/IPv4: 4 cases, 58 checks, 0 failures**: OS entropy, ephemeral
  reservations, native socket/process tables and interfaces.
- **IPv6: 2 cases, 41 checks, 0 failures**, executed, not skipped.
- **21 independent Windows-applicable headers** pass strict native compilation.
- CLI help/version exit 0; invalid/extra/ambiguous arguments exit 2.
- UI dummy/software smoke/capture and argument/error contracts pass; failed file
  opening returns 1 as specified. This is distinct from the native display test.
- **Real native backend:** `desktop-runtime: SDL=3004020 video=windows renderer=software`.
  Window creation, at least eight frames, screenshot and clean shutdown pass in Debug,
  Release and ASan. The extracted Release package is launched again natively.

Windows ASan instruments PLPT and directly compiled ImGui; the **official prebuilt
SDL DLL is not an instrumented vendor build**. Windows leak detection/UBSan/TSan are
not claimed. Linux's separately built SDL is instrumented in its own memory profile.

`build.bat fuzz` compiled and executed **20,000 native libFuzzer + ASan runs**, seed
20261113, maximum input length 4096, from two fixed corpus entries. Exit 0; this is
a codec fuzz smoke, not exhaustive protocol/network qualification.

`selftest` passed real missing-compiler, compiler-option, unresolved-symbol and
bad-archive failures, rejected stale successful outputs and preserved a deliberate
**test exit 23** exactly. No failed process or SKIP was converted into PASS.

## Actual runtime/package verification

The pinned SDL DLL imports only these 11 Windows OS DLLs:
`SETUPAPI`, `WINMM`, `IMM32`, `VERSION`, `KERNEL32`, `USER32`, `GDI32`,
`ADVAPI32`, `SHELL32`, `ole32`, `OLEAUT32`. No VCRUNTIME/MSVCP DLL import was
observed; the conditional app-local CRT-copy path was not needed by this pin.

Archive produced by the identified run:

```text
out/windows/packages/PLPT-0.1.0-preview-windows-x64.zip
SHA256 d6b6bc4c1bbbc3708272904c5801356a58b643d2aac5f778ed3616e539d9664f
```

The script extracted it into a new check directory, compared both EXEs, SDL3.dll,
README, dependency ledger and three notice files byte-for-byte, executed CLI
`--version`, then opened/closed the extracted desktop with the Windows video driver.
No test/model executable is staged. Only Release is packaged, not the ASan runtime
or Debug CRT. Microsoft redistribution terms still apply to embedded runtime code.

Full logs/captures/ZIP are in the workflow's **direct-windows-evidence** artifact,
ID **14037966613**, 2,785,861 bytes for the entire evidence bundle. Its separate
GitHub artifact digest is `9161bb187ff3bc08688ed703459151775527d9b26ad50912ffdd3179ac9d3ee8`;
this is **not** the application ZIP hash above. Retention expires **2026-11-27
12:38:23 UTC** unless the owner changes retention. Durable API excerpts/metadata and
checksums are committed with this report. Full redirected log/artifact storage is
unreachable from the sandbox; no local download/re-execution of Windows binaries
is claimed. Execution/extraction occurred on the actual Windows runner.

## Failure investigation, not hidden retries

Early native doctor attempts incorrectly re-read the VsDevCmd initialization banner
instead of compiler output from a reused capture. The raw failure text was retained
via temporary, same-repository PR evidence comments
([first](evidence/direct-build-2026-11-13/windows-actions/doctor-raw-comment.json),
[repeat](evidence/direct-build-2026-11-13/windows-actions/doctor-repeat-comment.json)).
This was a capture/validation bug, not proof that LLVM was invalid.

Fix: a separate SDK log and a **fresh file for every command**, with checked process
status and output existence. Version/target/SDK/probe requirements were not bypassed.
Subsequent full native jobs passed. The temporary PR-comment permission/step was
removed; final CI has read-only repository permissions plus artifact handling.
Annotations are aggregated to stay within GitHub's per-step/job limits; full command
logs are retained independently, including every diagnostic-mode invocation.

## Limits that remain

- Hosted native Windows is not manual testing on a physical Windows 10/11 client.
- Native software-renderer smoke does not qualify GPU/DPI/IME/multi-monitor or user interaction.
- asInvoker is verified; no claim that the hosted runner account was non-privileged.
- No signed installer, SmartScreen/clean end-user VM or comprehensive installation/uninstall audit.
- Fresh deterministic source/flag selection is proved; bit-identical PE/PDB/ZIP timestamps are not claimed.
- Product licensing/redistribution review, browser/QUIC, five guests, performance and cleanup gates remain separate.

**The native Windows build-migration gate is satisfied; the application remains a local monitor.**
