# Native Linux build validation

Date: 2026-11-13. Branch: `arena/da48a9c5-local-test-share`.

## Observed local results so far

Environment: native Linux x86-64, Debian 12/glibc 2.36/libstdc++ headers 20220819.
LLVM clang/clang++ 22.1.8 (`ca7933e47d3a3451d81e72ac174dcb5aa28b59d1`), target
`x86_64-unknown-linux-gnu`, linker `/usr/bin/ld`. Sandbox LLVM provenance and
SHA-512 checks are documented in BUILD_DEPENDENCIES.

```sh
export PATH=/home/user/.cache/plpt-direct-tools/node_modules/llvm-runtime-clang-linux-x64/bin:$PATH
bash -n build.sh
./build.sh deps --fetch
./build.sh doctor --headless
./build.sh debug --core-only
```

All four exited 0. The toolchain probe compiled/linked/executed; the eight core
objects and CLI linked cleanly under strict warnings. Core-only Debug measured
**9 seconds**, not a full desktop build. Both source archive hashes matched.

The first full direct SDL attempt reached linking but lacked the Unix GTK support
object; the second exposed missing `HAVE_GETRESUID/HAVE_GETRESGID` Linux feature
literals. These were implementation failures, not passing tests. After adding the Unix
support sources and Linux feature literals, `./build.sh test debug --headless`
completed successfully: **15/15 command contracts, zero skips**, 20 independent
headers, unit 26 cases/12,449 checks, platform 6/284, IPv6 2/41, SDL runtime
3004020 + dummy/software, screenshot success and error paths. The complete build
elapsed time is retained in `20261113T114520Z-test-debug-3264.log`.
Release/sanitizer/fuzzer/package/native X11 results remain pending. The dummy
result is not a native display or Windows qualification.

## Native runner qualification plan

The `ubuntu-24.04` job initializes clang and X11/Xvfb, then uses only `build.sh`:
`deps --fetch`, `doctor --verbose`, `debug --verbose`, `test debug --native-ui`,
`release --verbose`, `test release --native-ui`, `test asan --native-ui`, `fuzz`,
`selftest`, `clean`, `package --native-ui`. Xvfb initializes the display only.

The local `--headless` profile is not a substitute for an X11 window. Linux/Xvfb
can validate native X11 startup/render/capture/shutdown but not human interaction,
DPI, GPU or a physical monitor. Windows qualification is independent.
