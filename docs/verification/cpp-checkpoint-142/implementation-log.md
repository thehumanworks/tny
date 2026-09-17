# Implementation worker log

Scope: CP1–CP6 implementation and focused proof, CP7 build/CI/Nix wiring,
CP8 local measurement tooling. Coordinator owns broad verification, independent
review, evidence/review finalization and delivery. No commits or remote mutations.

Before implementation: read required project docs, both ADR0114 records,
ADR0124/0125 and the existing contract/evidence. Initial contract SHA-256:
`174316d68dca170aa16fdf0af7a3d97b77931405b62ffaeffb6d715816f78f3f`.
The existing full initial snapshot and evidence ledger preceded source edits.
Existing ADR hashes captured in `build/checkpoint-baseline/immutable.json`.
No separate worker goal or review agents: the coordinator assigned orchestration
and instructed the worker not to spawn additional agents. Design review read when available.

Baseline executable was not present in this worker environment. A separate
`BUILD=build/checkpoint-baseline` release build is being captured before edits;
all files remain inside this isolated worktree.

## Delivered implementation

- Replaced `src/core/checkpoint.c` with `checkpoint.cpp` (428 lines), retaining
  the C facade. All four entries catch exceptions. Checked field/array helpers,
  context/document owners and scoped secret buffers replace cleanup branches.
- Recovery performs one owned merged restore, retains refreshed credentials,
  checks saved-model identity without changing the caller, then constructs
  extensions. Public encoding never inserts private fields into its document.
- Hardened the necessary C extension construction/discovery dependency. A real
  injected discovery allocation previously reached `opendir(NULL)` under ASan.
  Required paths, entries, collision records and manager allocation now fail
  cleanly, with partially compacted entries still owned exactly once. No
  extension process is started by the fixture or identity check.
- Added the C fixture against `OWNER_LIB_OBJS` (full injected C/C++ graph), native
  CI and Nix gates, and four executable mutation oracles. The existing parser
  mutation driver accepts an optional mutant set/marker/label; its original
  default gate was rerun successfully.
- Added ADR0126 and minimal active language/architecture references. Immutable
  initial contract and every prior ADR remain byte-identical.

## Focused verification results

All commands run in this worktree, Apple clang 21 / macOS arm64. Exit 0 unless
explicitly called out below. These are worker checks, not coordinator final
acceptance or hosted-platform proof.

| Check | Result / retained log |
| --- | --- |
| `make -j8 release test-unit` | 567 passed, 32,022 assertions; `build/checkpoint-build.log` |
| `make -j8 test-checkpoint-ownership` | ASan/UBSan PASS; `build/checkpoint-owner.log` |
| `make -j8 test-checkpoint-ownership SANITIZE=0 BUILD=build/checkpoint-release` | PASS; `build/checkpoint-release-build.log` |
| `leaks --atExit -- build/checkpoint-release/checkpoint-ownership/checkpoint-test` | 0 leaks / 0 leaked bytes; `build/checkpoint-leaks.log` |
| `make test-checkpoint-mutation` | Valid baseline and 4/4 compiled, linked runtime assertion kills; `build/checkpoint-mutations/run-vmk1_a0b/report.json` |
| `make test-parser-mutation` | Shared-driver regression PASS, 4/4 kills; `build/parser-mutations/run-6eixb7pd/report.json` |
| `make analyze-cpp TIDY_SRC=src/core/checkpoint.cpp` | PASS; `build/checkpoint-tidy.log` |
| `make tidy TIDY_SRC=src/core/extensions.c` | PASS; `build/checkpoint-c-tidy.log` |
| `make warn-strict TIDY_SRC='src/core/checkpoint.cpp src/core/extensions.c'` | PASS; `build/checkpoint-strict.log` |
| Scoped clang-format, Ruff lint/format, `git diff --check` | PASS |
| Prior ADR and initial-contract SHA-256 comparison | PASS; initial hashes in `build/checkpoint-baseline/immutable.json` |

Independent schema assertions cover all 65 private keys and 55 public keys.
Full/empty/absent/null/long-field cases, every existing field, partial arrays,
malformed typed values, integer bounds, duplicate keys, destroyed input data,
encoder source lifetime, secret exclusion, permission/tool negative controls,
public null overrides/missing-field inheritance, repeated cycles, sticky caller
OOM and invalid source counts are exercised. Synthetic extension fixtures
include a file, package and name collision; restored managers stay dormant.

The fault sweep discovers and requires a hit at every allocation index:

| Operation | Full graph | Empty/default graph |
| --- | ---: | ---: |
| Private encode | 11 | 7 |
| Restore | 97 | 33 |
| Public encode/identity | 24 | 10 |
| Recover | 131 | 47 |

An additional 131-index saved-model/refreshed-credential recovery sweep checks
caller struct bytes, pointers and serialized semantics after success and every
failure. Total: 491 injected failures, all rejected. Successful operations
retain complete independent contexts. Raw C allocations are covered by host
leak checks in addition to the C++ owner counter.

Copy/default-identity mutants deliberately bypass both the immediate check and
its redundant sticky-OOM barrier; removing only one is equivalent. Every kill
in the checkpoint run exits 1 with a checkpoint assertion, not a compile error
or sanitizer crash.

Earlier failed attempts: the first compile used a nonexistent yyjson immutable
value-copy helper (fixed by checked write/parse with a wiping temporary). The
first fault sweep reproduced the extension discovery null-path ASan crash
(fixed and all indices rerun). These failures do not count as successful checks.

## Same-host measurements

Pre-edit baseline built with `make -j8 release BUILD=build/checkpoint-baseline`
at `4e760be09908d61e23029486e1c9a3b91fbfcbbe`; candidate uses the same release flags.
Baseline and candidate remain inside this worktree. No other checkout was read
or changed for builds.

- Stripped baseline: **1,121,184 bytes**; candidate: **1,104,688 bytes**.
- Both depend on system `libc++.1.dylib` and `libSystem.B.dylib`; no new runtime
  dependency. Full `otool -L` output is in `build/checkpoint-*-dependencies.txt`.
- `python3 tests/bench/bench_startup.py --baseline build/checkpoint-baseline/tny
  --candidate build/tny --output build/checkpoint-startup.json`: PASS. Medians
  before/after: version 3.034/3.008 ms; help 3.160/3.237 ms; PTY first prompt
  3.366/3.551 ms. Defaults retain 102 CLI samples and 20 PTY samples per artifact.
  Raw samples/hashes: `build/checkpoint-startup.json`.
- Identical `--bench` fixture, 2,000 private encode/restore/free cycles per run,
  six alternating runs per artifact: median CPU time **0.145639 s baseline**,
  **0.151699 s candidate** (approximately 4.2% slower). This is not a speedup.
  Raw CPU samples and `/usr/bin/time -l` memory data are in
  `build/checkpoint-workload.json`. The helper
  `build/checkpoint-baseline/bench_compile.py` links the same fixture against
  the actual baseline/candidate release object graphs, replacing only alloc.c
  with its test-introspection build. This does not measure every libc allocation.

## Design review disposition and coordinator handoff

Read `/Users/tomas/.cache/tny-cpp-next-20260917/design-review.txt` when it became
available. Its checked-copy, wiped temporary, model-header accumulation,
caller-preservation, array-bound and typed-field concerns are covered above.
The suggested split retaining encoders in C and a caller rollback guard conflict
with the user's explicit single conversion/unchanged-caller scope; the owned
merged-context design implements that scope instead. Separate broader recovery
policy concerns and other ranked conversions remain outside #142.

Current worker-owned input hashes are in `build/checkpoint-source-manifest.json`.
The coordinator still owns full `make test`, global `make quality`, restart /
background integrations, complete ABI/allocation regressions, Linux/wasm/MSYS/
Nix and hosted evidence, independent implementation review, final contract and
evidence reconciliation, commits/push/PR. Those gates were not silently waived
or represented as complete here. No commit, push, GitHub mutation, live-provider
call, subagent or other-checkout operation was performed.
