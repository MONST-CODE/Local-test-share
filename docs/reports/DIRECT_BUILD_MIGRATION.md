> **Superseded Windows toolchain profile.** The Microsoft ABI/SDK/VC results below are historical, not MSYS2 UCRT64 validation. The owner now requires only `PLPT_MSYS2_ROOT\ucrt64\bin\clang++.exe`. See [UCRT64 migration](UCRT64_TOOLCHAIN_MIGRATION.md), [native validation](UCRT64_WINDOWS_VALIDATION.md) and [dependencies](UCRT64_DEPENDENCIES.md). Linux results remain separately scoped.

# Direct native build migration — reconnaissance and execution record

Date: **2026-11-13**. Owner decision: **mandatory; no Jenga or external build engine**.

**Result: the direct-build P0 gate is qualified on native Windows and Linux.**
Final code/scripts: **13fd4642f629bc2ca6182e663fa487ee1d9da113**;
[run 43426058514](https://github.com/MONST-CODE/Local-test-share/actions/runs/43426058514),
both jobs **completed / success**. This report/evidence follow-up is documentation,
not a new build implementation or sharing feature.

## Read-only reconnaissance (recorded before structural changes)

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

## Implementation delivered on the feature branch

The read-only reconnaissance above was committed first (`d4816ed`), before the
implementation commit (`5187c10`). Both authoritative scripts now own explicit
PLPT/model/ImGui/SDL inputs, includes, flags, direct compiler/linker invocations,
tests and packages. There is no generated project, helper build executable or
indirect invocation of a forbidden engine. No incrementality is attempted.

Removed active definitions/helpers: CMakeLists/presets/cmake directory, CMake test
runners, Python header/cross-syntax/package/corpus helpers, PowerShell packager and
the unqualified NSIS recipe. Corpus seeds are tiny fixed test inputs. The supplied
Windows manifest and SDL/ImGui notices remain; X11-specific upstream notices were
added. The immutable master document is not altered. Prior preview reports are
explicitly historical and their original evidence is retained.

Windows directly selects LLVM clang++.exe with Microsoft x64 target, SDK/STL/CRT,
lld-link, resource compiler and native ABI/runtime probe. Official SDL VC ZIP is
preferred and hash checked; no MinGW fallback. Linux directly compiles a pinned
SDL source profile because no official Linux development binary is published.
ImGui compiles only the core plus SDL3 and SDLRenderer3 backends. Application
models remain test-only. Neither script configures/builds an upstream project.

Every compilation clears its target directory first, measures duration and retains
commands/diagnostics and compiler dependency files. `BUILD_SUCCEEDED` is written
only after the target links. Tests preserve actual exits; expected negative exits
do not waive sanitizer diagnostics; unavailable prerequisites yield explicit SKIP
or failure. Packaging rebuilds/tests Release and checks the extracted runtime.

CI contains native Windows/Linux environment initialization, invocations of these
scripts and evidence handling only. It does not define sources or flags itself.
Focused commits are pushed only to `arena/da48a9c5-local-test-share`. New draft PR:
https://github.com/MONST-CODE/Local-test-share/pull/2 . It remains available for the
owner's review; no automatic merge, force push or main modification.

## Final acceptance matrix — actual native execution

The final run identifies branch head **13fd4642f629bc2ca6182e663fa487ee1d9da113**.
GitHub's PR checkout merge commit is **b54d19c6c7dcc3af2676048d6206a42498697150**;
its tree **e9ca484b59c4a3f04aa7d2c62bdae1f282c29945** is identical to the branch
head's tree, verified through the GitHub API and local Git. Thus this is not an
unidentified merge-only source snapshot. All required native job steps succeeded.

| Gate | Native Windows x64 | Native Linux x64 |
|---|---|---|
| Toolchain / `doctor` | Clang++/LLD **20.1.8**, Microsoft ABI, SDK **10.0.26100.0**, VC **14.44.35207**, native probe passed | Clang **18.1.3**, C++20, glibc **2.39**, GNU ld **2.42**, native probe passed |
| Clean Debug / Release | Both CLI and SDL/ImGui UI compiled and linked | Both CLI and SDL/ImGui UI compiled and linked |
| Direct Debug / Release tests | **15 passed / 0 skipped** per mode | **16 passed / 0 skipped** per mode |
| Native window / shutdown | **windows/software**; not just dummy | **x11/software** via Xvfb; not just dummy |
| Unit / platform / IPv6 | **26/12,449**, **4/58**, **2/41** cases/checks, zero failures | **26/12,449**, **6/286**, **2/41**, zero failures |
| Memory / error paths | ASan, **15 / 0**; native UI, CLI/UI errors and capture-open failure | ASan+UBSan, **16 / 0**; native UI, same errors plus `/dev/full` |
| Independent native headers | **21**, strict warnings | **20**, strict warnings |
| Native libFuzzer | **20,000 executions**, ASan, exit 0 | **20,000 executions**, ASan+UBSan, exit 0 |
| Failure propagation / stale outputs | Compiler/compile/link/hash failures + exact test exit **23**, passed | Same categories + exact test exit **23**, passed |
| `clean`, rebuild and `package` | Fresh Release + **15 / 0** + ZIP extraction/comparison/native launch | Fresh Release + **16 / 0** + TGZ extraction/comparison/native launch |
| Dependency pin checks | Actual official VC ZIP + ImGui SHA-256 match, x64 COFF/PE/runtime checked | Actual SDL/ImGui source SHA-256 match, directly built SDL runtime checked |

Counts in the test rows are **command contracts**, not the number of individual
unit assertions. The detailed platform reports list exact commands, environment
versions, measured durations, outputs and limitations:

1. [BUILD_VALIDATION_WINDOWS](BUILD_VALIDATION_WINDOWS.md) — priority platform;
   native Microsoft build/link/run, SDK/STL/CRT/manifest, 15-contract configurations.
2. [BUILD_VALIDATION_LINUX](BUILD_VALIDATION_LINUX.md) — native X11, 16-contract
   configurations and separately labelled historical dummy-only sandbox results.
3. [BUILD_DEPENDENCIES](BUILD_DEPENDENCIES.md) — pinned URLs/digests/layouts,
   ABI/runtime requirements, license inventory and update/regression rules.
4. [Evidence index](evidence/direct-build-2026-11-13/README.md) — raw API records,
   readable annotation excerpts, local logs, provenance and checksum manifests.

### Reliability and output policy

- Required `debug`, `release`, `test`, `clean`, `package`, `doctor` are implemented
  and exercised on **each** OS. Explicit `deps --fetch`, `fuzz` and `selftest` are
  additional operations of those same scripts, not helper engines.
- Outputs stay under `out/{windows,linux}/{debug,release,tests,packages}/`, with
  separate dependency, doctor and retained log directories.
- Each compilation deletes its target objects, executables and success marker
  before validation. There is **no incremental object cache**; all source/header,
  flag, dependency and compiler changes are rebuilt by construction. Compiler `.d`
  files are retained for inspection, not claimed as an incremental scheduler.
- Measured times include validation/extraction/fresh compilation/linking/header
  checks, not tests. Windows records centiseconds, Linux whole seconds. Fresh
  deterministic input selection does **not** promise byte-identical PE/PDB/archive
  timestamps or immutable hosted OS toolchains.
- Compiler/link commands and output are retained; `--verbose` additionally exposes
  all invocations and compiler-driver diagnostics. Exit codes are checked directly,
  missing output fails, and sanitizer output is not accepted even for expected
  error-path exits. SKIP/77 is incomplete qualification, never PASS.
- Native scripts use fixed cleanup boundaries and reject unsafe output
  symlinks/reparse points. Logs and verified archives survive `clean`. Locks prevent
  ordinary overlapping builds; never edit a running script.
- CI has no duplicated source/flag lists and no external project build engine.
  Temporary Windows diagnostic PR-write permission/comments were removed before
  the final qualification; final workflow repository permission is `contents: read`.

### Qualified packages — hashes are run-specific

At **13fd464**, the native jobs produced, extracted, compared and executed:

```text
PLPT-0.1.0-preview-windows-x64.zip
SHA256 d6b6bc4c1bbbc3708272904c5801356a58b643d2aac5f778ed3616e539d9664f

PLPT-0.1.0-preview-linux-x64.tar.gz
SHA256 67fc3199ec0a1657299ceb75949f498bda88736e450baf676ea235fa55ce859f
```

These are portable **development archives**, not signed installers. They contain
CLI/UI, the SDL runtime, guides/dependency ledger and notices, not model/test
executables. Documentation-only changes after 13fd464 leave the compiled source
and script logic unchanged but can change package documentation and archive bytes;
**the hashes above must not be attributed to a later rebuild**.

The run's `direct-windows-evidence` / `direct-linux-evidence` artifacts contain the
archives and full logs/captures. Their IDs, digests, sizes and expiry are recorded
separately: an evidence-bundle digest is not an application-package digest. Retention
expires **2026-11-27 UTC**. Save artifacts through GitHub before expiry if the full
bundles are needed. The sandbox cannot reach the redirected Actions storage host;
no downloaded bundle or local Windows execution is invented. Durable API
annotations confirm the actual native script outcomes and are committed with
local logs; they are **not** substitutes for bytewise local inspection of unavailable
remote artifact contents.

## Failure history and corrective work

Failed runs remain failures; the final pass does not rewrite them.

| Run / branch code | Observed failure or result | Resolution / status |
|---|---|---|
| [43419873428](https://github.com/MONST-CODE/Local-test-share/actions/runs/43419873428), `0eb74a7` | Windows doctor; Linux native Debug | Preserved initial step/annotation evidence; native iteration continued |
| [43420470875](https://github.com/MONST-CODE/Local-test-share/actions/runs/43420470875), `6c7a93d` | Windows doctor; Xlib `XGenericEventCookie` conflict | Explicit modern-Xlib config and X11 source list |
| [43421015102](https://github.com/MONST-CODE/Local-test-share/actions/runs/43421015102), `94d0b39` | Windows doctor; missing `SDL_Zenity_ShowMessageBox` | Explicit X11 support objects; no vendor patch/build generator |
| [43421520604](https://github.com/MONST-CODE/Local-test-share/actions/runs/43421520604), `0b43280` | Windows doctor; Linux X11 ASan (Debug/Release already passed) | Memory failure investigated without suppression |
| [43422582021](https://github.com/MONST-CODE/Local-test-share/actions/runs/43422582021), `85b47bc` | Windows capture failure; run cancelled | Temporary raw doctor evidence exposed stale VsDevCmd banner |
| [43422895103](https://github.com/MONST-CODE/Local-test-share/actions/runs/43422895103), `b679360` | Repeated doctor capture; **37-byte** Xlib title leak | Separate SDK/per-command captures; compile-time ASCII OS title |
| [43423683934](https://github.com/MONST-CODE/Local-test-share/actions/runs/43423683934), `bf89e8e` | **First complete native Windows + Linux success** | Historical first pass, not the final package revision |
| [43426058514](https://github.com/MONST-CODE/Local-test-share/actions/runs/43426058514), `13fd464` | **Complete native success again** | Includes finalized scripts, package guide/notices, runtime metadata and bounded evidence |

The Windows doctor bug reused stale process-output captures; validation was fixed,
not bypassed or replaced with MinGW. The exact inherited-handle cause is not proven.
The Linux title workaround avoids a pinned SDL/Xlib allocation leak without vendor
patches or disabling LSan; Unicode within ImGui is unchanged. The separate BMP
failure regression remains covered by explicit IOStream ownership and close checks.
Both platform reports retain the relevant diagnostics and scope of the fixes.

The baseline Windows failure before migration remains of unknown cause because its
full logs were inaccessible; it is **not** retroactively attributed to these bugs.

## Scope, preserved master and hand-off

`docs/ARCHITECTURE_MASTER.md` is unchanged from the base commit; verified SHA-256:

```text
25e6fac9e2ddf081ccdfe6d0a8542a12ea2406499cbf840b3aee581373b47b39
```

The delivered application is still **only a local monitor**. No sharing, browser
runtime, QUIC, new network listener, fixed data port, tunnel or third-party service
was added. This migration removes the build-architecture blocker; it does not
satisfy or bypass the product gates for five guests, authorization/revocation,
source/resource non-transfer, cleanup, capture or latency.

Remaining qualification boundaries: human DPI/IME/GPU/multi-monitor testing,
standard-user clean client installations, signing/SmartScreen, cross-distribution
Linux ABI, long-duration fuzz/concurrency and product licensing review. Windows
ASan does not instrument the official prebuilt SDL DLL; its unused app-local
CRT-copy branch is not claimed as tested. There is no universal installer or
bit-reproducible release claim.

**No next sharing/network feature has been started. PR #2 is not merged; the owner
retains the review and next-lot decision.**
