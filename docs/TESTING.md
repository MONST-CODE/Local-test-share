# Tests through the direct native scripts

There is no CTest or Python/PowerShell test orchestrator. `build.bat` and `build.sh`
compile each test executable directly, check the link output, execute it and preserve
its exit code and log. `test` always compiles fresh; it never trusts old executables.

```bat
build.bat test debug --native-ui
build.bat test release --native-ui
build.bat test asan --native-ui
build.bat fuzz
build.bat selftest
```

```sh
./build.sh test debug --native-ui
./build.sh test release --native-ui
./build.sh test asan --native-ui
./build.sh fuzz
./build.sh selftest
# Explicit sandbox-only profile (not native display qualification):
./build.sh test debug --headless
```

Preserved suites:

- `tests/unit/test_main.cpp`: types/results, ancestry, registry incompleteness and
  capacity, immutable snapshots/queue bounds, local session/role/context rules,
  DM privacy/quotas/UTF-8, protocol bounds/mutations, audit and unavailable backends.
- `tests/integration/platform_tests.cpp`: OS RNG, live dynamically bound TCP/UDP
  sockets, table ownership/processes/interfaces, IPv4 and IPv6; Linux proc fixtures
  and FD release. Tests use port zero, not a fixed development listener.
- CLI help/version/invalid/extra/ambiguous arguments: exact exit statuses.
- Desktop startup/shutdown, actual SDL header/runtime version, capture success,
  bad arguments, invalid video driver and failed output opening. Linux additionally
  tests `/dev/full`. Sanitizer diagnostics fail even an expected exit-1 error test.
- Native window smoke: `video=windows` or `video=x11`, software renderer; separate
  from `video=dummy`. This is not a human DPI/IME/input or GPU qualification.
- ASan on Windows only when the exact UCRT64 compiler-rt runtime is present (otherwise
  explicit SKIP 77, never PASS); ASan+UBSan on Linux. Linux directly built SDL/ImGui
  are also instrumented in this profile; the UCRT64 SDL3 DLL is prebuilt and not an
  instrumented vendor binary. SDL/ImGui on Windows are checked as x64 GNU/UCRT ABI.
- libFuzzer codec roundtrip, 20,000 executions with two checked-in tiny valid corpus
  entries, deterministic seed, 4096-byte input cap. This is a short fuzz smoke, not
  exhaustive protocol or video-payload fuzzing.
- `selftest`: missing UCRT64 compiler, misleading `MSYSTEM=CLANG64`, rejected standalone
  compiler override, compile/link failures, corrupt archive rejection, no stale output
  marker/binary, and exact propagation of a deliberate test exit 23.
- `ide-check`: native validation of the VS Code task/config contract; it does not
  claim interactive editor-extension/UI qualification.
- Native Windows CI also exercises each exact `cmd.exe /d /c build.bat <task>` chain,
  embedded asInvoker/PerMonitorV2 manifest inspection, and extracted-package execution
  after removing MSYS2 from PATH.

`77` means **SKIP / incomplete qualification** for unavailable IPv6/instrumentation;
it is not counted as PASS. Required CI steps return non-zero on that condition.
The checked-in historical counts are not automatically results for this migration.
See the native platform reports for the actual current counts, versions and runs.

CI uses fresh native OS checkouts, invokes only the authoritative scripts for project
operations, removes generated outputs, then rebuilds and tests an extracted package.
Native desktop launch on hosted Windows or X11/Xvfb is distinguished from manual
interaction on a physical display. No five-guest/LAN result is implied by any build test.
