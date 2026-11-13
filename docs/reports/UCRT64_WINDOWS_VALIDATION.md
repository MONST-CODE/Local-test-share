# Native Windows MSYS2 UCRT64 validation

Date: 2026-11-13. Branch: `arena/4197f0b0-local-test-share`.
Base: `3b62b48585c2ce095cca4681a2aad4f466fec85f`.

**Pending native execution.** Earlier MSVC/SDK results are superseded-profile
history, not UCRT64 proof. The local editing host is Linux. No cross-build result
will be counted as native Windows validation.

Planned native commands (ordinary cmd, no Developer Command Prompt):

```bat
call build.bat deps --fetch
call build.bat doctor --verbose
call build.bat ide-check
cmd.exe /d /c build.bat debug
cmd.exe /d /c build.bat test
call build.bat test debug --native-ui
cmd.exe /d /c build.bat release
call build.bat test release --native-ui
call build.bat test asan --native-ui
call build.bat fuzz
call build.bat selftest
cmd.exe /d /c build.bat clean
call build.bat package
```

ASan/libFuzzer are conditional on the exact UCRT64 runtime's availability; missing
support is recorded SKIP, never counted as successful memory/fuzz execution.

The final report must record compiler path/version/target, actual include/library
search paths, MinGW-w64 UCRT and GNU libstdc++ identity, native process/socket/RNG
and port-reservation tests, Debug/Release/native-window/error-path results, compiler
and linker failure propagation, manifest extraction checks, package DLL closure,
extraction/execution with no MSYS2 on PATH, and fresh-checkout/clean rebuild proof.

VS Code JSON must parse and the native task/config contract must pass. All four
exact task command chains above are exercised in native CI. This is separate from
manual clicking, IntelliSense extension-host operation, DPI/IME/GPU and owner-machine
installation; those must not be described as executed unless actually tested.
