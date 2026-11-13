# Native Windows build validation

Date: 2026-11-13. Current branch: `arena/da48a9c5-local-test-share`.

**Status: implementation ready for native CI; not yet qualified.** No native
Windows build or execution from this migration has been observed yet. The old
syntax-only checks and old failed workflow runs do not count as Windows proof.

The authoritative script is `build.bat`. The native `windows-2022` job will execute:

```bat
build.bat deps --fetch
build.bat doctor --verbose
build.bat debug --verbose
build.bat test debug --native-ui
build.bat release --verbose
build.bat test release --native-ui
build.bat test asan --native-ui
build.bat fuzz
build.bat selftest
build.bat clean
build.bat package
```

Required evidence: actual LLVM/Microsoft SDK/STL/linker versions, x64 probe,
verified VC ZIP hash, SDL import/DLL architecture and runtime imports, direct
Debug/Release compiles, unit/platform/IPv4/IPv6/errors/ASan/fuzzer, real
`video=windows` launch/shutdown, embedded manifest, clean rebuild and extracted
ZIP execution. Each step retains `out/windows/logs`, per-test logs and captures
in a Windows-only workflow artifact. Unsupported instrumentation/IPv6 is SKIP/77,
not PASS. The qualification report will be updated from actual runner results.

Known boundaries independent of the build: no manual DPI/IME/GPU or physical-PC
interaction proof, no signed installer, no clean end-user VM audit, no LAN sharing.
