# Delivery reconciliation — evidence-only support

**Overall task: INCOMPLETE.** This audit changed only new files in this directory.
It did not build, install, start workers/agents, stage, commit, push, mutate goals,
change a worktree, or signal an existing process. Primary owns integration and gates.
Observations span 2026-09-12 13:07–13:16 UTC; these are snapshots, not a source freeze.

## Verified preservation and ownership

| Work completed | Checks and results | Blockers / limits |
| --- | --- | --- |
| Immutable initial contract | SHA-256 `832e97ffd3ba8b5e21a60a356e13b8759e89efae348cac5f9fa41682fb31c6b1` matches the separate recorded hash. | Current contract amendments are distinct; initial bytes not rewritten. |
| Baseline ADR directory | **88/88 filenames and hashes match**, including README. New ADRs 0087–0096 inventoried. | Prefixes **0030 and 0045 each occur twice**. I-G6/C-G6 remains blocked. |
| Worktrees | 18 registered trees. Original checkout and all nine pre-existing auxiliaries have unchanged metadata and clean status; task baseline is also clean. | Canonical plus six worker trees contain task changes. Status does not certify unseen historical activity. |
| Branch/source identity | Canonical is `feat/durable-image-workflows`, HEAD `b80c04b9df740c8388da03991cf4808c07e9cb50`. 1,101 non-evidence source records captured initially. | `tests/test_core.c` changed during this audit. Primary integration is active; repeat affected gates on final inputs. |
| Native goal | Actual `codex app-server` initialize + **thread/goal/get** reads `01a091ec-193f-7a91-b8e3-b28e757eff19`: **active**, **49 invariant IDs**, tokenBudget null. Exact schema and requests saved. | No set, clear, start, resume, new goal, or completion request sent. App-server exited after stdin EOF. |
| Execution chain | Retained record 36 does not pass ordinary self-hash validation. Existing preimage reconstructs its hash exactly and matches the resume-file hash. | Preserve this constructor defect and its disposition; do not call the chain an unexplained all-green ordinary chain. |

Evidence: [immutable.json](immutable.json), [worktrees.json](worktrees.json),
[source-canonical.json](source-canonical.json), [source-drift-at-close.json](source-drift-at-close.json),
[goal-read.json](goal-read.json), [goal-read-schema.json](goal-read-schema.json),
[chain36-disposition.json](chain36-disposition.json).

## Worker reconciliation and integration hazards

Each worker's recorded owned-file hash was independently compared with its actual
worktree and canonical. Counts include explicitly recorded unchanged files.

| Worker | Recorded files still match worker | Equal to initial canonical bytes | Evidence / consequence |
| --- | --- | --- | --- |
| A13 manifest corrections | **30/30** | **1/30** | `worker-manifest.json`; owned plans and retained-error changes require primary merge and fresh code review. |
| Exports/contact sheets | **27/27** | **0/27** | `worker-exports.json`; do not overwrite corrected A13 shared callers with worker before-images. |
| A14 jobs corrections | **31/31** | **0/31** | `worker-jobs.json`; actual later retry/credential corrections exist; earlier defects are not assumed still present. |
| A15 captured queue | **25/25** | **1/25** | `worker-preview.json`; captured queue is a prerequisite, not generated-result preview completion. |

[worker-overlap.json](worker-overlap.json) names **21 overlapping paths**, including
`tools.c`, `tests/test_core.c`, manifests, interception, CLI/help, docs and Nix/CI.
[worktree-source-fingerprints.json](worktree-source-fingerprints.json) binds all eight
task worktrees to source hashes and baseline-relative file records. Never merge
whole worktrees indiscriminately. All historical worker counts need rechecking if
an owner writes again. Live process paths identify current task sessions and QEMU;
older worker completion records do not mean there are no current owners.

## Raw results and stale evidence

- [record-index.json](record-index.json) inventories **685** run-like JSON records.
  **627 referenced log digests match retained bytes; zero mismatches.** Hash identity
  proves retention, not correctness or current-tree coverage. [raw-results-index.json](raw-results-index.json)
  reads/indexes **158** selected raw logs without relaying diagnostic payloads.
- F008 raw log says **26 tests, OK**. F009 quality, F011 ABI and F012 leaks report
  exit 0 on their own manifests. Their existing source entries matched the initial
  canonical capture; only later ADR files were absent. F013's whole source manifest
  matched that capture. This is useful historical evidence, not a completed combined
  feature gate. The later `test_core.c` change invalidates affected current claims.
- F010 full Mac test reports exit 0 **and source_unchanged:false**. Its disposition
  attributes that in-run change to an ADR and requires a frozen rerun. Keep both facts.
- Jobs raw T001 fails `test_parser_map_covers_dispatch`; the missing jobs help/source
  inventory remains an integration requirement unless primary has now fixed it.
  Scoped worker result is **47 tests: 44 pass, 3 skips**, not 47 passing tests.
  Two skips are wasm-only; real manifest lineage/retry integration is the third.
- Jobs mutation record has **17 compiled kills**, with restored builds/oracles.
  Exports has **7 kills and survivor X125.7a**, not eight kills. Bounded converter
  timeout remains unrun. Resolve or independently justify the survivor.
- A13 mutation history preserves an earlier **5/9** result with four survivors and
  two final-oracle failures; its later separate run records **9/9**, restored clean.
  The separately reported late-cancel fault must remain separately bound; do not
  replace earlier failures with the later verdict.
- Preview faults.json contains **8 cases (F003–F010)**; F001/F002 live in separate
  fault/restoration logs. Worker claim of ten must include all ten, not array length.
  Preview full-test/leak failures and claimed restored-base reproduction remain
  unresolved combined-tree gates. Nine queue cases passing do not deliver #126.
- Historical Linux dictation timeout, generic mutation coverage, actual browser-WASM
  readiness/mutation, native Windows acceptance, Nix and final release-size gates
  remain open until source-bound final proof. Prior Linux size was 1,051,608 B against
  1,048,576 B; baseline 986,072 B and isolated LTO 917,216 B do not measure this final tree.
- Existing evidence/ledger opening statuses lag late worker outputs. Existing records
  were read, not edited. Primary must append delivery authorization and final verdicts
  without changing the initial contract or treating PR authorization as an ADR waiver.

See [mutation-reconciliation.json](mutation-reconciliation.json). The task contract
requires behavioral fault failures; the generic mutation skill's timeout-as-kill
advice does **not** override this stronger task-specific criterion.

## Platforms and processes: usable routes, not acceptance

- Mac read/exec and pinned quality tools work. Apple Clang 21, clang-format 23.1,
  clang-tidy 22.1.8, Ruff 0.16.6, ShellCheck 0.11, shfmt 3.14 and actionlint 1.7.12.
  Python 3.14.7 and Node 26.8.1. No host Nix/emcc/magick/Valgrind on PATH.
- Default Docker context query failed. **Task context works**:
  `docker --context colima-tny-open-issues-20260911 ps` finds running
  `tny-verification-20260911` and `tny-nix-verification-20260911`.
  Both are Linux aarch64. Existing GCC/Clang/Valgrind/tini, emsdk 6.0.8 and browser
  venv paths were observed; Nix 2.35.2 path exists. No container/build was started.
  Existing orphan Python processes remain untouched; ownership/reaping needs primary
  care before new platform runs. Browser executable/runtime checks were not run here.
- Task-private **ImageMagick 7.1.2-31** executes `-version` at
  `/Users/tomas/.cache/tny-verification/open-issues-20260911/imagemagick-macos/prefix/bin/magick`.
  Missing PATH entry is not absent tooling; use the explicit documented executable.
- **QEMU PID 65737**, uid 501, PPID 1, executable and cwd under the existing task's
  `windows-runtime` cache. QMP reports 11.1.1 and running. Listeners are loopback
  `127.0.0.1:2222` (SSH) and `:5901` (VNC). No signals or state-changing QMP methods.
- SSH with the existing task-only key and known_hosts, strict host-key verification,
  updates disabled, no user agent/config, successfully returned Windows
  **10.0.26200.8037**. First `cmd /c ver` failed due to the documented OpenSSH quoting
  issue; corrected bare `ver` succeeded. Both attempts retained.
  Guest is Windows ARM64 with x86_64 MSYS2 under x64 emulation, **not native x86_64 CI**.
  No tny build/test occurred. The existing substrate probe remains only substrate proof.

Evidence: [platform-tools.json](platform-tools.json),
[platform-task-context.json](platform-task-context.json), [processes.json](processes.json),
[qemu-readonly.json](qemu-readonly.json), [qemu-guest-query-correction.json](qemu-guest-query-correction.json).

## Safe staging and precise blockers

Follow [stage-policy.md](stage-policy.md). Never `git add .` for this tree.
The retained scan covers 2,984 files / 91,879,386 bytes. No current environment
secret value, private-key material, JWT, or whole-environment-dictionary line was
found in its defined text scan; that is **not** an exhaustive secret-free guarantee.
A token-shaped baseline fixture and other sensitive-literal candidates remain held.
Worker-reported deletion of an earlier environment dump cannot establish absence
from prior sessions/tool logs. Do not echo, delete, or assert no prior exposure.

Required decisions/work remain:
1. **User decision, I-G6:** recommend grandfathering only baseline 0030/0045 collisions,
   preserving their bytes and requiring unique new numbers. Tradeoff: two legacy
   collisions remain. Renumbering instead needs explicit preservation relaxation.
   No exception is currently authorized.
2. **Primary integration/reviews:** merge owned A13, export, corrected jobs, captured
   queue deltas, then complete generated preview and cross-feature lineage/retained
   failure behavior. Resolve actual findings on the merged bytes.
3. **Primary gates:** jobs help inventory, skipped lineage, preview failures, export
   survivor/timeout, final native/macOS/Linux/MSYS2/browser-WASM/Nix/SDK/ABI/leak/
   analyzer/mutation/live-provider and size proof. Do not substitute available ARM
   substrates, stale hosted CI or fixture success for a required missing boundary.
4. **Delivery hygiene:** dispose held fixture matches safely; preserve excluded raw
   archives locally or in a separately approved artifact store. File-only pointers
   to excluded archives are not portable PR evidence. Re-scan final staged inputs.

## Reproduction and audit limitations

`audit.py` documents the inventory algorithms and performs no builds. It creates
outputs exclusively and refuses to overwrite them. To reproduce without writes,
rehash `contract.initial.md`, every `baseline.json["adrs"]` path and each indexed
log; compare the recorded SHA-256 values. Read platform/goal JSON `argv`/`requests`
for the exact read-only checks and schema. Do not run an existing task helper that
launches reviews, changes goals, builds, or appends the canonical execution chain.

Loaded whole HANDOFF/AGENTS/544-line contract, installed verification-contract and
define-goal skills, and repo mutation skill. `tny skill show` is unsupported here;
installed skill files were used. No product C was written. Installed `tny edit`
rejects empty SEARCH and cannot create a new file directly: exclusive one-line
placeholders were necessary; **all substantive output content used `tny edit`**.
`audit.py` inventory was later compacted as JSON without data changes; its initial
pretty-form digest is reconstructible from the retained data and audit-run.json.
