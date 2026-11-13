# Direct native build migration — reconnaissance and execution record

Date: 2026-11-13. Owner decision: **mandatory; no Jenga or external build engine**.

## Read-only reconnaissance (before structural changes)

- Inspected checkout, clean working tree, scripts, source/target lists, test contracts,
  packaging, dependency pins, CI and active documentation.
- Current working branch: `arena/da48a9c5-local-test-share`, already based on
  `73162d9b857abd0841bdbb116b49e94fb98cdca6` of `main`. This is the dedicated session
  feature branch; no second branch will be created or main modified.
- Baseline has real CMake/Ninja/CTest/CPack orchestration behind thin .bat/.sh wrappers.
  These definitions and Python/PowerShell build helpers must be removed from the
  active path, not renamed or hidden behind another wrapper.
- Preserve: C++20, clang++, x86-64, strict application warnings, Debug/Release,
  Linux ASan/UBSan, native platform sources, isolated model tests, explicit no-sharing
  product scope, SDL3/ImGui versions and license notices.
- Windows intent: Microsoft SDK/STL ABI, static application CRT (`/MT`, `/MTd`),
  ws2_32/iphlpapi/bcrypt, asInvoker/PerMonitorV2 manifest; not MinGW.
- Test contracts currently include unit/platform/IPv6, CLI argument status codes,
  dummy UI/error paths, BMP failure/leak regression and libFuzzer roundtrip.
- SDL3 release 3.4.20 provides an official VC development archive but **no official
  Linux x64 binary development archive** in its release asset list. Windows VC ZIP
  SHA-256 published by the release API: `1ec3009655ee37b1f73e52b3b120acc5afd8abaf9532f3303b1bf1dfe91b5969`.
  Linux must use a transparent directly compiled source profile unless a separately
  qualified binary is available; no dependency build generator may be invoked.
- Source archives remain pinned to SDL `f00dc919ca35b9756316e86c4777392de442f9b2`
  and ImGui `c1aaac65e896a45f81f47a4d68f9959e0f86c859`, with the existing hashes.
- This fresh Linux sandbox has no clang++, X11 development headers or previous
  build cache. Tool provisioning is separate from the project build lifecycle.
- The baseline Windows Actions runs are marked failed by GitHub. Their log download
  redirected to a host unavailable in this sandbox, so the reason is **not yet known**.
  No earlier Windows success is assumed.

## Implementation plan / gates

1. Put all actual source lists, flags, direct compile/link commands, dependency
   verification, tests, clean and packaging operations in `build.bat` / `build.sh`.
2. Use deterministic fresh object/output trees for every compilation, retained logs,
   explicit return codes, measured duration and diagnostic command echoing.
3. Provide explicit pinned dependency provisioning (never a floating implicit build
   download). Build ImGui core + SDL3/SDLRenderer3 backends directly. Prefer official
   SDL VC binaries on Windows; document the Linux direct-source configuration.
4. Migrate test orchestration into the same two scripts. No CTest, Python or
   PowerShell build/test/package helpers remain active.
5. Replace CI with native runners that only initialize prerequisites, invoke the
   two scripts and retain platform-separated evidence. Exercise clean-checkout,
   negative/failure, sanitizers, fuzz and real native window smoke where possible.
6. Produce the three platform/dependency reports plus this final migration report,
   focused commits and a new PR. Never merge automatically.

## Current qualification state

Reconnaissance complete. Implementation and native qualification are pending.
No build/test from the replacement architecture is claimed successful yet.
No next major sharing/network feature is authorized by this migration.
