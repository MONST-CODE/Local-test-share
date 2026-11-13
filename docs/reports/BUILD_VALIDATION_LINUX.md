# Native Linux x64 — direct-build qualification

Date: **2026-11-13**. Status: **PASS for the build-migration gate**.
The native X11 qualification below is distinct from the earlier dummy-only sandbox tests.

## Evidence identity

- Branch: `arena/da48a9c5-local-test-share`, base main `73162d9b857abd0841bdbb116b49e94fb98cdca6`.
- Qualified code/scripts: **`13fd4642f629bc2ca6182e663fa487ee1d9da113`**.
- [Run 43426058514](https://github.com/MONST-CODE/Local-test-share/actions/runs/43426058514): **both native jobs succeeded**.
- Linux job **131887629947**, runner **ubuntu-24.04**, native x86-64.
- The PR merge checkout's tree `e9ca484b59c4a3f04aa7d2c62bdae1f282c29945` was
  verified identical to the qualified branch commit's tree. Saved source/merge
  identity is in the [evidence directory](evidence/direct-build-2026-11-13/README.md).
- [Readable native excerpts](evidence/direct-build-2026-11-13/linux-actions/qualification-excerpts.log),
  [raw annotations](evidence/direct-build-2026-11-13/linux-actions/qualification-annotations.json),
  [job/step results](evidence/direct-build-2026-11-13/qualification-jobs.json).

The first complete Windows/Linux pass was run
[43423683934](https://github.com/MONST-CODE/Local-test-share/actions/runs/43423683934)
at `bf89e8e`. The later run requalified the final scripts, portable-package guide,
notices and diagnostic handling. Report-only follow-up does not change C++ sources
or build scripts; these recorded package hashes belong to **13fd464**, not to later
archives containing revised documentation.

## Actual native toolchain and environment

| Item | Observed value |
|---|---|
| OS / runner label | Ubuntu 24.04, `ubuntu-24.04`, x86-64 |
| Kernel | **7.0.0-1017-azure**, Ubuntu |
| Compiler / target | **Ubuntu clang 18.1.3 (1ubuntu1)**, `x86_64-pc-linux-gnu` |
| Linker | `/usr/bin/ld`, **GNU Binutils 2.42** |
| Native C++ probe | C++ `202002`, pointer size 8, libstdc++ headers **20240908** |
| glibc / libc6 package | **2.39** / **2.39-0ubuntu8.9** |
| libstdc++6 package | **14.2.0-4ubuntu2~24.04.1** |
| libX11 / libXext packages | **2:1.8.7-1ubuntu0.1** / **2:1.3.4-1build2** |
| Display / renderer | **X11 via Xvfb**, SDL software renderer |

`doctor` directly compiles, links and runs the C++20/thread/native architecture
probe. X11 headers and the selected compiler/target are checked before compilation.
The default profile directly compiles pinned SDL **3.4.20** C sources and ImGui
**1.93.0** core + SDL3/SDLRenderer3 backend objects. No upstream build generator is
run. Dependency archive identities/hashes are in [BUILD_DEPENDENCIES](BUILD_DEPENDENCIES.md).

## Commands actually executed on the native runner

The workflow initializes OS prerequisites, then delegates every project operation
to `build.sh`. Xvfb supplies a display, not build definitions.

```sh
sudo apt-get update && sudo apt-get install -y clang libx11-dev libxext-dev xvfb xauth
./build.sh deps --fetch
./build.sh doctor --verbose
xvfb-run -a ./build.sh test asan --native-ui
./build.sh debug --verbose
xvfb-run -a ./build.sh test debug --native-ui
./build.sh release --verbose
xvfb-run -a ./build.sh test release --native-ui
./build.sh fuzz
./build.sh selftest
./build.sh clean
xvfb-run -a ./build.sh package --native-ui
```

All required steps succeeded; none was skipped. The memory mode is intentionally
first. Debug still starts with a fresh Debug output tree; no ASan objects are reused.
After `clean` removes compiled outputs, `package` performs another fresh Release
build, complete test run and extraction/runtime check.

## Measured builds and actual tests

| Invocation | Fresh build time | Actual result |
|---|---:|---|
| `test asan --native-ui` | **98 s** | **16 passed, 0 skipped**, ASan + UBSan |
| `debug --verbose` | **64 s** | CLI + SDL/ImGui desktop linked |
| `test debug --native-ui` | **79 s** | **16 passed, 0 skipped** |
| `release --verbose` | **88 s** | CLI + SDL/ImGui desktop linked |
| `test release --native-ui` | **104 s** | **16 passed, 0 skipped** |
| Release rebuild inside `package`, after `clean` | **104 s** | **16 passed, 0 skipped**, archive extracted and executed |

These script measurements include validation, extraction, fresh compilation/linking
and applicable independent-header checks, but **not test execution**. They are
host-specific whole-second measurements, not estimates or LAN performance claims.

Per tested configuration:

- **Unit: 26 cases / 12,449 checks / 0 failures**.
- **Platform/IPv4: 6 cases / 286 checks / 0 failures**: OS RNG, live ephemeral
  TCP/UDP sockets, ownership/process/interface inspection, proc fixtures and FD release.
- **IPv6: 2 cases / 41 checks / 0 failures**, actually executed.
- **20 independent applicable headers**, strict native compilation.
- CLI help/version exit 0; unknown/extra/ambiguous arguments exit 2.
- Dummy/software UI render/capture/shutdown, argument checks and invalid driver/file
  paths pass. `/dev/full` exercises a write/close failure and returns the required 1.
- **Native UI:** `desktop-runtime: SDL=3004020 video=x11 renderer=software`.
  Window creation, at least eight frames, screenshot and clean shutdown pass in
  Debug, Release and ASan+UBSan. The extracted Release package is launched again.

ASan+UBSan instruments **PLPT, ImGui and the directly compiled SDL library**. Host
libc/libstdc++/Xlib remain system binaries. Sanitizer diagnostics fail even a test
whose expected application exit is 1; no leak suppression or waived error is used.
No TSan result is claimed.

`fuzz` executed **20,000 native libFuzzer + ASan/UBSan runs**, exit 0, fixed seed
20261113, maximum input length 4096, starting from the two checked-in 56/58-byte
seeds. This is a codec smoke campaign, not exhaustive network/security qualification.

`selftest` verified missing-compiler, invalid compile/link option and corrupt-archive
rejection, absence of stale success markers/executables, and exact propagation of
a deliberate **exit 23**. Its visible `FAIL ... intentional-exit` message is the
expected negative case; the parent checks that failure and then reports SELFTEST
PASS. The entire step returned 0; no ordinary failed test was silently accepted.

## Package/runtime evidence

Archive generated by the identified run:

```text
out/linux/packages/PLPT-0.1.0-preview-linux-x64.tar.gz
SHA256 67fc3199ec0a1657299ceb75949f498bda88736e450baf676ea235fa55ce859f
```

The script extracted into a fresh check directory; compared CLI, UI, SDL library,
README, dependency ledger and three notices byte-for-byte; ran CLI `--version`
and the extracted desktop using X11/software. No model/test executable is staged.
`ldd` resolves `libSDL3.so.0` from the package's own `bin/` directory, alongside
system libstdc++/libgcc_s/libm/glibc/X11/Xext and transitive XCB/Xau/Xdmcp/bsd/md.
This is a host-ABI package, **not a universal Linux binary**.

The **direct-linux-evidence** artifact is ID **14038272702**, 1,816,963 bytes for
the whole evidence bundle, digest
`7bbfaa226ff0a665d7b7de168d4210cf582170b6976c614fa2fe930130aec769`.
That artifact digest is **not** the TGZ hash above. Retention expires **2026-11-27
12:42:38 UTC** unless changed by the owner. Logs/captures/TGZ were uploaded by the
successful job. Redirected Actions storage is unreachable from the sandbox, so
this report relies on retained API annotations/metadata for remote execution,
not a claimed local download or independent inspection of the uploaded bundle.

## Earlier sandbox execution — supplementary, dummy-only

Native Debian 12 x86-64, glibc **2.36**, GNU ld **2.40**, libstdc++ headers **20220819**.
LLVM **22.1.8** (`ca7933e47d3a3451d81e72ac174dcb5aa28b59d1`), target
`x86_64-unknown-linux-gnu`, was provisioned separately with verified SHA-512 from
npm LLVM tool packages, without lifecycle scripts. This secondary-distribution
limitation and exact digests are in
[tool-provenance.txt](evidence/direct-build-2026-11-13/linux-local/tool-provenance.txt).
No X11 development headers/display were available here.

```sh
export PATH=/home/user/.cache/plpt-direct-tools/node_modules/llvm-runtime-clang-linux-x64/bin:$PATH
bash -n build.sh
./build.sh deps --fetch
./build.sh doctor --headless
./build.sh debug --core-only
./build.sh test debug --headless
./build.sh test asan --headless
./build.sh fuzz
./build.sh test release --headless
./build.sh selftest
./build.sh clean
./build.sh test debug --headless --verbose
```

| Successful local invocation | Measured build | Preserved log under `linux-local/` |
|---|---:|---|
| Core Debug | **9 s** | `20261113T113023Z-debug-debug-1468.log` |
| Full dummy Debug | **98 s**, 15 passed / 0 skipped | `20261113T114520Z-test-debug-3264.log` |
| Full dummy ASan+UBSan | **117 s**, 15 / 0 | `20261113T114812Z-test-asan-4770.log` |
| Full dummy Release | **139 s**, 15 / 0 | `20261113T115222Z-test-release-6406.log` |
| libFuzzer | 20,000 executions, exit 0 | `20261113T115037Z-fuzz-debug-6286.log` |
| Isolated selftest | Passed | `20261113T115612Z-selftest-debug-7999.log` |
| Debug after clean | **96 s**, 15 / 0 | `20261113T121059Z-{clean-debug-10536,test-debug-10579}.log` |
| Final-script doctor/selftest | Passed | `20261113T123224Z-doctor-debug-12347.log`, `20261113T123225Z-selftest-debug-12425.log` |

The earlier full builds are development history, not execution of the final
13fd464 source tree. The last doctor/selftest ran with the script changes committed
as 13fd464. Full final-code coverage comes from the native qualification above.
Local unit/IPv6 counts were 26/12,449 and 2/41; platform counts were 6/284 on this
host. The native runner's observed 286 checks are recorded separately, not inferred
from sandbox results. Dummy-only mode intentionally omits the native-display case
and is explicitly unsuitable for packaging.

## Resolved failures and regression protection

1. Early local SDL link failure `SDL_Gtk_Quit` and glibc `getresuid/getresgid`
   declaration conflicts were fixed in explicit source/config lists. Failed local
   logs `20261113T113044Z-test-debug-1740.log` and `20261113T114112Z-test-debug-2833.log`
   remain in evidence. A selftest overlapped an editor write (`Text file busy`);
   that invalid attempt was followed by an isolated, successful execution.
2. Native X11 exposed the modern-Xlib generic-event guard and missing
   toolkit/xsettings/EDID/Zenity support objects. Those are explicit inputs in
   `build.sh`; associated license notices are retained. No vendor build generator.
3. In run 43422895103, LSan detected **37 leaked bytes** in the Xlib title-conversion
   path through pinned SDL's `SDL_X11_SetWindowTitle`. See
   [raw diagnostic](evidence/direct-build-2026-11-13/linux-actions/x11-title-leak-annotations.json).
   The unrepresentable em dash reached a pinned SDL path that omitted freeing
   `XTextProperty.value` when conversion returned a positive value. The OS window
   title is now compile-time-checked ASCII; ImGui Unicode remains unchanged. No
   vendor patch or sanitizer suppression. Both complete native runs then passed
   X11 ASan/UBSan. Re-evaluate this workaround when updating SDL.
4. The independent BMP failure/leak regression remains covered: RAII surface,
   explicitly opened IOStream, `SDL_SaveBMP_IO(..., false)` and checked close.
   Impossible-open and `/dev/full` tests must continue to pass with instrumentation.

## Remaining limits

X11/Xvfb startup/render/capture/shutdown is not human input, DPI/IME/GPU or a
physical-monitor test. No Wayland-only, other distribution/libc, standard-user
installation audit, five-guest sharing, transport, cleanup-after-crash or LAN
performance result is claimed. Deterministic input selection/fresh rebuilds do
not imply bit-identical binaries or archive timestamps.

**The Linux build-migration gate is satisfied; Windows has its own independent native proof.**
