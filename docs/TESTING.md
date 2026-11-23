# Tests through the direct native scripts

There is no CTest or Python/PowerShell test orchestrator. `build.bat` and `build.sh`
compile each test executable directly, check the link output, execute it and preserve
its exit code and log. `test` always compiles fresh; it never trusts old executables.

```bat
./build.bat test debug --native-ui
./build.bat test release --native-ui
./build.bat test asan --native-ui
./build.bat fuzz
./build.bat selftest
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

Reading the console: the test step prints one line per suite (cases and checks
from the harness summary, or the SKIP reason), then grouped CLI and desktop
checks. A failing suite shows its assertions, evidence lines and summary, raw,
followed by the path of its complete capture. Full logs: `out/windows/logs/` and
`<build dir>/<suite>.log`. See `docs/BUILD.md`, « Lire la sortie du build ».

Preserved suites:

- `tests/unit/test_main.cpp`: types/results, ancestry, registry incompleteness and
  capacity, immutable snapshots/queue bounds, local session/role/context rules,
  DM privacy/quotas/UTF-8, protocol bounds/mutations, audit and unavailable backends,
  feedback payload bounds and the protocol 1.5 `SessionInfo` host block (ADR-019);
  version negotiation is derived from `kProtocolMinor`.
- `tests/integration/platform_tests.cpp`: OS RNG, live dynamically bound TCP/UDP
  sockets, table ownership/processes/interfaces, IPv4 and IPv6; Linux proc fixtures
  and FD release. Tests use port zero, not a fixed development listener. The real
  control plane (`lan_control_plane_host_and_guest`) also covers chat, private
  messages guest↔guest, guest→host and host→guest (echo to the author, invisible
  to third parties) and feedback reports (local refusal, host registry, severity,
  context, acknowledgement visible to the author only).
- `tests/integration/browser_pipeline_tests.cpp` (Windows, `devlink-browser-tests`):
  real WebView2 context, navigation to the `/fixture` of `devlink-demo-service`, run
  id read back FROM PIXELS, click -> changed frame, origin policy and SOCKS5 egress
  guard (canary port must see 0 connections), clean teardown.
- `tests/integration/share_pipeline_tests.cpp` (Windows, `devlink-share-tests`):
  production share path on loopback: control plane with challenge-response + TLS 1.3,
  `MediaOffer`, per-guest TLS 1.3 PSK media channel, WIC decode. Mode B exclusive
  control and transfer, Unicode text, mode C refusal of valid records from a
  modified client, mode A separate contexts/cookies/localStorage, hostile peers
  (wrong key, usurped id, other session, junk before TLS, TLS 1.2 downgrade, replay,
  oversized record), refused unsafe starts with rollback, start/stop cycles, five
  guests.
- `tests/integration/desktop_e2e_tests.cpp` (Windows, `devlink-desktop-e2e-tests`):
  the SHIPPED executables (it opens real, focused windows and moves the real mouse
  cursor: do not use the mouse while it runs). `devlink-demo-service`,
  `devlink-desktop --host`, then
  `devlink-desktop --join` in a real SDL3 window. Synthetic SDL mouse/text events go
  through ImGui, the letterbox mapping, the media channel and the host policy. The
  host DOM must show the click and the exact Unicode text, the guest's own window
  capture must contain the fixture square in the colour of a click count the host
  really reached (`clicks mod 4`; external clicks are reported as evidence) (a frame without a
  drawable surface — minimised or hidden window — is never saved as evidence: the
  window is restored and the capture deferred, and the evidence line records the
  window state), both processes
  exit 0 and every browser context is destroyed. Single machine: **not** a LAN
  qualification (see `docs/reports/REAL_LAN_QUALIFICATION.md`).
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

`77` means **SKIP / incomplete qualification** for unavailable IPv6/instrumentation
or a missing WebView2/TLS runtime (browser, share, desktop-e2e suites);
it is not counted as PASS. Required CI steps return non-zero on that condition.
The checked-in historical counts are not automatically results for this migration.
See the native platform reports for the actual current counts, versions and runs.

CI uses fresh native OS checkouts, invokes only the authoritative scripts for project
operations, removes generated outputs, then rebuilds and tests an extracted package.
Native desktop launch on hosted Windows or X11/Xvfb is distinguished from manual
interaction on a physical display. No five-guest/LAN result is implied by any build test.
