# MSYS2 UCRT64 Windows evidence index — 2026-11-13

Real execution records for the P0 Windows toolchain correction (owner-mandated
MSYS2 UCRT64 `clang++.exe`, no Microsoft SDK, no MSVC). This directory contains no
generated binaries, no upstream source archives and no simulated native output.
Reports distinguish a build/test gate from product qualification.

## Primary qualification snapshot

| Identity | Value |
|---|---|
| Branch | `arena/4197f0b0-local-test-share` |
| Base main | `3b62b48585c2ce095cca4681a2aad4f466fec85f` |
| Qualified branch head | **`498d18d9855488d903a563438c6798385ab15bb2`** |
| PR | [#3](https://github.com/MONST-CODE/Local-test-share/pull/3), draft, open, `merged=false` |
| Run | [43456570072](https://github.com/MONST-CODE/Local-test-share/actions/runs/43456570072), attempt 1, **completed / success** |
| Windows job / check | **131992341565**, `windows-2022`, **success** |
| Linux job / check | **131992341961**, `ubuntu-24.04`, **success** |
| PR checkout (GitHub test merge) | `e5b0993806b4d7bf6124ca0f7fae7630b3062732`, parents `3b62b48` + `498d18d` |
| Test-merge tree = `git rev-parse 498d18d^{tree}` | `21c8096091dfa7676ebab19fb35a5b4930a0d6c3` — identical |

The PR checkout is a GitHub-generated test merge, **not an assistant merge into
main**. Identical trees prove the runner compiled exactly this head's tracked
inputs. No main change, history rewrite, force push or branch switch occurred.

Native facts recorded by that run: compiler
`C:\msys64\ucrt64\bin\clang++.exe`, `clang version 23.1.5`, target
`x86_64-w64-windows-gnu`, GNU ld 2.47.20260726, GNU libstdc++ 16.2.0, UCRT via
`api-ms-win-crt-*`, SDL3 3.4.20-1 from `mingw-w64-ucrt-x86_64-sdl3`, ImGui 1.93.0
compiled directly, Debug/Release builds, 15/15 native tests including a real Win32
window, extracted package executed with a system-only PATH.

### Files and provenance

- `qualification-run.json`, `qualification-jobs.json`, `qualification-artifacts.json`:
  saved GitHub API responses for `/actions/runs/43456570072` and its `/jobs` and
  `/artifacts` endpoints, collected after completion.
- `qualification-pr-source.json`, `qualification-merge-tree.json`: identity fields
  from `/pulls/3` and `/git/commits/e5b0993806b4d7bf6124ca0f7fae7630b3062732`,
  recorded **at collection time**, not a live assertion about a later head.
- `windows-actions/qualification-annotations.json`: **44** real check annotations
  from `/check-runs/131992341565/annotations`, i.e. the script's own summaries —
  toolchain identity, probe output, runtime closures, test summaries, selftest,
  package digest.
- `linux-actions/qualification-annotations.json`: **32** real annotations from
  `/check-runs/131992341961/annotations` (Linux non-regression).
- `windows-actions/qualification-excerpts.log`,
  `linux-actions/qualification-excerpts.log`: readable projections of those JSON
  annotations, reversed into execution order. Message text is quoted verbatim.
  These are **excerpts**, not full job logs.
- `BUILD_INPUTS.sha256`: SHA-256 of the normalized Git blobs at the qualified head
  for every input the direct build reads or copies — `.gitattributes`, the
  workflow, `.vscode/` contract files, `build.bat`, `build.sh`, `app/`, `engine/`,
  `tests/`, `packaging/`, `third_party/`, and the documentation files copied into
  packages. Not a binary checksum list.
- `SHA256SUMS`: integrity inventory of the other files in this directory, relative
  to it; the checksum file excludes itself.

### Uploaded evidence bundles

| Artifact | ID / bytes | GitHub-reported digest | Expires (UTC) |
|---|---|---|---|
| `ucrt64-windows-evidence` | **14053704998** / **23,030,751** | `sha256:4a91ea17701d24fa5a5e779d6ec57d83e778cce91415a95d7ddad7fc0373109f` | 2026-11-27T16:00:30Z |
| `direct-linux-evidence` | **14055555707** / **1,816,127** | `sha256:23e93335d391b49918dc2b1fd424760cff74f1408755d59b286ff77a85e3c055` | 2026-11-27T16:01:01Z |

Windows package built and executed by the script itself:
`PLPT-0.1.0-preview-windows-ucrt64-x64.zip`, SHA-256
`8b8618e2a8ab719aa0bcb17821b72cfe024b1743d3370ca462c5b0675d697c00`. Linux:
`PLPT-0.1.0-preview-linux-x64.tar.gz`, SHA-256
`683558bd8bc657045f3f96cba3c895cce4fd230a2b0d0b49f9975a5fa6891628`.

**Access limitation:** `api.github.com` requests succeed, but complete Actions log
and artifact downloads redirect to storage hosts unreachable from this editing
sandbox (EOF). The bundles were **not downloaded or independently inspected here**;
their upload status, metadata and the annotations above are real API evidence. No
local Windows execution is claimed by the assistant. Save the bundles before expiry
if the full logs, screenshots (`desktop.bmp`, `desktop-native.bmp`) and command
captures are needed.

## Iteration history — real failures kept, not erased

Each entry names the run, the head it qualified, and the observed cause.

| Run | Head | Windows outcome | Retained files |
|---|---|---|---|
| 43447400424 | `0a61c81` | First UCRT64 attempt; failed early because `findstr /X` equality checks on pacman/`.PKGINFO` output are LF/CRLF-sensitive | `first-run-in-progress.json`, `first-windows-annotations.json` |
| 43448693091 | `cbceae1` | Doctor abort: `foreign toolchain path/target in compiler diagnostics` — clang's `-v` log prints legacy GCC probe directories such as `C:/msys64/ucrt64/x86_64-w64-mingw32/usr/include` inside `ignoring nonexistent directory` lines | `cbceae1-doctor-foreign-path-jobs.json`, `windows-actions/cbceae1-doctor-foreign-path-annotations.json` |
| 43449945849 | `bf26a1a` | Same false positive; search-dir and include-path checks were narrowed but the rejection scan still covered the whole `-v` log | `bf26a1a-doctor-foreign-path-jobs.json`, `windows-actions/bf26a1a-doctor-foreign-path-annotations.json`, `run-bf26a1a-windows-initial.json` |
| 43450748153 | `a156a1c` | Doctor and IDE contract passed; Debug compile failed with `C:/msys64/ucrt64/include/c++/16.2.0/cstdlib:83:15: fatal error: 'stdlib.h' file not found` — a duplicated `-isystem ucrt64/include` broke libstdc++'s `include_next` chain | `a156a1c-stdlib-include-duplication-jobs.json`, `windows-actions/a156a1c-stdlib-include-duplication-annotations.json` |
| 43452161886 | `abe3f18` | Debug linked the console monitor, then the GUI binary failed: `libmingw32.a(…crtexewin.o): undefined reference to 'WinMain'` | `abe3f18-winmain-link-jobs.json`, `windows-actions/abe3f18-winmain-link-annotations.json` |
| 43456365307 | `3689067` | Contained the `-municode` fix and libFuzzer decoupling; **cancelled** by the workflow concurrency group because a documentation-only commit was pushed while it ran: `Canceling since a higher priority waiting request for plpt-direct-refs/pull/3/merge exists` | `3689067-cancelled-by-doc-push-jobs.json`, `windows-actions/3689067-cancelled-by-doc-push-annotations.json` |
| **43456570072** | **`498d18d`** | **First complete native UCRT64 qualification: both jobs success** | `qualification-*.json`, `*-actions/qualification-*` |

Process lesson recorded by the cancelled run: never push to the branch while a
qualification run is in flight, or the concurrency group cancels the evidence.

### Commits after the snapshot head

The commit that adds this directory also changes one thing in `build.bat`: the
doctor's libFuzzer capability probe now retains the compiler's own failure output
(`FUZZ_REASON`, emitted as an evidence annotation) instead of a generic message. It
changes no compile, link, package or test flag, no source list and no gate; the
capability result stays SKIP either way. The run triggered by that push
re-qualifies that head, and the snapshot above is then refreshed to it in a
docs/evidence-only commit. `build.sh`, `.vscode/`, `app/`, `engine/`, `tests/`,
`packaging/`, `third_party/` and the workflow are untouched after `498d18d`.

Because the two package-copied UCRT64 reports change with these documentation
commits, **do not apply the ZIP digest above to an archive built from later
documentation**. No bit-for-bit reproducible archive claim is made.

## Superseded Microsoft profile

`docs/reports/BUILD_VALIDATION_WINDOWS.md`, `DIRECT_BUILD_MIGRATION.md`,
`BUILD_DEPENDENCIES.md`, the `LOT_*`/`VALIDATION` reports and
`docs/reports/evidence/direct-build-2026-11-13/` document the earlier
MSVC/Windows-SDK/VC-SDL3 toolchain. They are retained, marked superseded, and are
**not** UCRT64 evidence. Their Windows results do not qualify the owner's
toolchain requirement.

## Verifying retained evidence

From the repository root on a host with `sha256sum`:

```sh
(cd docs/reports/evidence/ucrt64-2026-11-13 && sha256sum --check SHA256SUMS)
```

To re-derive the build-input digests for any head:

```sh
git ls-tree -r --name-only <head> | grep -E '^(\.gitattributes|\.github/workflows/build\.yml|\.vscode/|app/|engine/|tests/|packaging/|third_party/|build\.(bat|sh)$|docs/(BUILD|QUICKSTART|SECURITY)\.md$|docs/reports/(BUILD_DEPENDENCIES|UCRT64_DEPENDENCIES|UCRT64_WINDOWS_VALIDATION)\.md$)' \
  | while read -r f; do printf '%s  %s\n' "$(git cat-file blob "<head>:$f" | sha256sum | cut -d' ' -f1)" "$f"; done
```
