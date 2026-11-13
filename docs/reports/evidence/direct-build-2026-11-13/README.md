# Direct-build evidence index — 2026-11-13

This directory retains real execution records for the build migration. It does not
contain generated binaries, upstream source archives or simulated native test output.
The reports distinguish a successful build/test gate from product qualification.

## Primary qualification snapshot

| Identity | Value |
|---|---|
| Branch | `arena/da48a9c5-local-test-share` |
| Base main | `73162d9b857abd0841bdbb116b49e94fb98cdca6` |
| Qualified branch head | **`13fd4642f629bc2ca6182e663fa487ee1d9da113`** |
| PR | [#2](https://github.com/MONST-CODE/Local-test-share/pull/2), not merged |
| Run | [43426058514](https://github.com/MONST-CODE/Local-test-share/actions/runs/43426058514), attempt 1, **completed / success** |
| Windows job/check | **131887629384**, `windows-2022`, **success** |
| Linux job/check | **131887629947**, `ubuntu-24.04`, **success** |
| PR checkout merge | `b54d19c6c7dcc3af2676048d6206a42498697150` |
| Identical merge/head tree | `e9ca484b59c4a3f04aa7d2c62bdae1f282c29945` |

The PR checkout is a GitHub-generated test merge, **not an assistant merge into
main**. Its parents were base main and the qualified head. Comparing its Git tree
to local `git rev-parse 13fd464^{tree}` proved identical tracked inputs. No main
change, history rewrite or branch switch was needed.

### Files and provenance

- `qualification-run.json`, `qualification-jobs.json`, `qualification-artifacts.json`:
  saved GitHub API responses for `/repos/MONST-CODE/Local-test-share/actions/runs/43426058514`
  and its `/jobs` and `/artifacts` endpoints, collected after completion.
- `qualification-pr-source.json`, `qualification-merge-tree.json`: selected identity
  fields from `/pulls/2` and `/git/commits/b54d19c6c7dcc3af2676048d6206a42498697150`.
  These record the PR state **at collection**, not a live assertion about its later head.
- `windows-actions/qualification-annotations.json`: **41** actual check annotations
  from `/check-runs/131887629384/annotations`, including direct script summaries.
- `linux-actions/qualification-annotations.json`: **32** actual check annotations
  from `/check-runs/131887629947/annotations`, including direct script summaries.
- Each platform's `qualification-excerpts.log` is a readable projection of its JSON
  annotation titles/messages, reversed into execution order. The heading identifies
  the run; message text is not fabricated. These are **excerpts**, not full job logs.
- `BUILD_INPUTS.sha256`: SHA-256 of normalized Git blobs at the qualified head for
  scripts, workflow/attributes, application/engine/tests, manifest/package guide,
  notices and documentation copied into packages. It is not a binary checksum list.
- `SHA256SUMS`: integrity inventory for all other files in this evidence directory,
  relative to this directory; the checksum file excludes itself.

The final code/scripts and notices are qualified at 13fd464. The following commit
refreshes reports/architecture/security documentation and adds evidence, without
changing C++ or build logic. In particular, package-copied SECURITY/dependency
ledger documentation can differ from 13fd464. Do **not** apply that run's package
hash to a new archive built from later documentation, even if executable sources
are unchanged. No bit-for-bit reproducible archive claim is made.

## Application archives versus evidence bundles

Application package hashes, **reported by native scripts after packaging**:

| Application archive | SHA-256 |
|---|---|
| `PLPT-0.1.0-preview-windows-x64.zip` | `d6b6bc4c1bbbc3708272904c5801356a58b643d2aac5f778ed3616e539d9664f` |
| `PLPT-0.1.0-preview-linux-x64.tar.gz` | `67fc3199ec0a1657299ceb75949f498bda88736e450baf676ea235fa55ce859f` |

Each job extracted, compared and executed its own package. GitHub uploaded separate
**evidence bundles** containing logs, captures and those archives:

| Evidence artifact | ID / bytes | GitHub-reported artifact SHA-256 | Expiry UTC |
|---|---|---|---|
| `direct-windows-evidence` | **14037966613** / **2,785,861** | `9161bb187ff3bc08688ed703459151775527d9b26ad50912ffdd3179ac9d3ee8` | **2026-11-27 12:38:23** |
| `direct-linux-evidence` | **14038272702** / **1,816,963** | `7bbfaa226ff0a665d7b7de168d4210cf582170b6976c614fa2fe930130aec769` | **2026-11-27 12:42:38** |

**Access limitation:** API requests to `api.github.com` succeed, but complete Actions
log/artifact downloads redirect to storage hosts unreachable from this sandbox
(EOF). The uploaded bundles were **not downloaded or independently inspected here**.
Their upload success, metadata and execution summaries are real API evidence;
no local Windows execution is claimed. Users with GitHub artifact access should
save the bundles before expiry. Committed excerpts/metadata remain after artifact
retention, but cannot reproduce every line/image/byte of the unavailable bundles.

## Historical development evidence — not final qualification

- `first-pass-{run,jobs,artifacts}.json`: completed successful run **43423683934** at
  **bf89e8e16522f5d2d46a2cd0b9cd187d93797147**. Both native platforms passed before
  the final notices/package/log polish.
- `*-actions/first-full-pass-annotations.json`: snapshots from that first successful
  run. The Windows snapshot has **50 entries**; it is not claimed to be a complete
  annotation archive. The final 13fd464 qualification records are separate.
- `seventh-jobs-running.json`: deliberately retained intermediate snapshot of the
  same run, Linux still in progress. **Superseded** by `first-pass-jobs.json`.
- `initial-{run,jobs}.json`, platform `initial-annotations.json`: failed initial
  native migration run **43419873428**, head **0eb74a7**.
- `third-jobs.json`, platform `third-annotations.json`: failed run **43421015102**,
  head **94d0b39**; Linux's missing SDL Zenity symbol and Windows doctor diagnostics.
- `fourth-jobs.json`, platform `fourth-annotations.json`: failed run **43421520604**,
  head **0b43280**; Linux Debug/Release passed but native memory gate failed.
- `sixth-jobs.json`: failed run **43422895103**, head **b679360**. Its Linux diagnostic
  is `linux-actions/x11-title-leak-annotations.json`, including the **37-byte** leak.
- `windows-actions/doctor-raw-comment.json` and `doctor-repeat-comment.json`: actual
  temporary PR evidence comments **6852417866** and **6852480029**, recovered because
  full redirected logs were unavailable. They expose stale VsDevCmd banner captures.
  The PR-comment CI permission/step was removed before final qualification.
- `linux-local/*.log`: direct sandbox command logs, including failed iterations,
  successful dummy Debug/Release/ASan+UBSan, 20,000-run fuzzer, clean/rebuild and
  deliberately failing selftest children. These are **not native-display evidence**.
- `linux-local/tool-provenance.txt`: actual SHA-512 verification of sandbox-only
  LLVM 22.1.8 npm tarballs. No lifecycle scripts were executed. This secondary
  distributor is documented, not conflated with the native Windows toolchain.

Earlier local full builds predate 13fd464; the latest local doctor/selftest uses
its script changes. The primary final native run supersedes the old in-progress
qualification claims. Expected selftest failures are labelled and checked by their
parent; other failed logs are not silently counted as passing tests.

## Verifying retained evidence

From the repository root on a host with `sha256sum`:

```sh
(cd docs/reports/evidence/direct-build-2026-11-13 && sha256sum --check SHA256SUMS)
```

`BUILD_INPUTS.sha256` uses **Git blob bytes**, not working-tree line endings:
`.gitattributes` checks `build.bat` out as CRLF on Windows. Verify against the
qualified Git objects, for example:

```sh
ref=13fd4642f629bc2ca6182e663fa487ee1d9da113
while read -r expected path; do
    actual=$(git show "$ref:$path" | sha256sum)
    test "$expected" = "${actual%% *}" || exit 1
done < docs/reports/evidence/direct-build-2026-11-13/BUILD_INPUTS.sha256
```

Hashes ensure retained-file consistency; they do not turn CI summaries into an
independent runtime audit. See the [migration report](../../DIRECT_BUILD_MIGRATION.md)
and separate [Windows](../../BUILD_VALIDATION_WINDOWS.md) / [Linux](../../BUILD_VALIDATION_LINUX.md)
reports for commands, counts, durations, resolved failures and untested boundaries.
