# Default automatic learning: verification

Date: 2026-09-19. Decision: [ADR 0154](../../adr/0154-default-automatic-workflow-learning.md).
Contract: [default self-improvement](../../instruction-improvement.md).

## Default behavior, not a selected preset

The native learner runs inside normal `tny ask` and TUI turns. The regression
fixture uses no `--task`, Python improvement controller, manual promotion or
extension. Two ordinary tasks supply recovery evidence. A later fresh session
receives the resulting advice automatically. Failed trials can demote it.

The implementation uses typed edit/read outcomes before tool-result formatting
or extension replacement. Tests include a successful read whose contents begin
with `error:`, terminal-profile intercepted edits, unrelated targets and intent,
unknown/schema-invalid calls, quoted tilde operands, opt-out, ephemeral isolation,
and the real TUI. Unit tests cover bounded state, persistence, corruption,
permissions, concurrency, nonblocking flush and cross-turn pending deltas.

## Measured replay

[`benchmark.json`](benchmark.json) records actual native tool responses, file
hashes, request counts, injected advice and workspace evidence snapshots. All
normal-case tool outcomes are checked against the real executor's responses.
Byte-exact file contents—not assistant completion text—determine task success.

The model is a deterministic local HTTP fixture. It retries blindly until the
learned advice is actually present, then reads before retrying. This makes the
control path measurable without inference spend. It **does not** prove that a
real model will follow the advice or improve on general coding tasks.

Each arm runs two ordinary warmup tasks per tool profile (`all`, `terminal`,
`terminal+edit`), then four unseen tasks per profile. Evaluation is **online**:
normal feedback continues after each case. It is not a frozen held-out
statistical generalization study. No future case content enters learner state.

| Online evaluation, 12 tasks | Pre-change binary | Current, disabled | Current, default |
| --- | ---: | ---: | ---: |
| Exact file checks passed | 12/12 | 12/12 | 12/12 |
| Verified native tool calls | 48 | 48 | 36 |
| Mock provider HTTP requests | 60 | 60 | 48 |
| Failed exact-edit attempts | 24 | 24 | 12 |

On this replay, default learning reduces tool calls **25%**, mock HTTP requests
**20%**, and failed edit attempts **50%**, without losing a file check. The six
warmup tasks cost **24 tool calls and 30 requests in every arm**. Including warmup,
the default uses 60 tool calls and 78 requests versus 72 and 90 for each control.
Setup, compiler work and independent oracle reads are excluded. There are no
latency, token, dollar, physical-I/O or live-model performance claims.

The pre-change binary was retained before implementation and matches the SHA-256
in the [preceding verification record](../instruction-improvement/checks.json):
`a73774eb2a5408a21f35eace3fba6cebaa7628500959bc7bd02642d7244ef761`.
It reports `0.17.0-7-gea4bfcc` and is the verified build before automatic learning.
The disabled-current control separates the learner from unrelated binary changes.

## Reproduction

```sh
mise install
make release
python3 tests/integration/test_default_learning.py
python3 tests/bench/bench_default_learning.py --out /tmp/default-learning.json
python3 tests/mutation/default_learning.py --out /tmp/learning-mutations.json
```

For the additional pre-change arm, build the earlier source in a separate tree:

```sh
git worktree add /tmp/tny-before-auto 9614e2c
make -C /tmp/tny-before-auto release
python3 tests/bench/bench_default_learning.py \
  --baseline /tmp/tny-before-auto/build/tny --out /tmp/default-learning-with-baseline.json
```

The benchmark refuses an existing output file. Paths, session IDs, source-build
version and platform metadata can differ on another machine. Compare the summary
and exact file outcomes, not byte-for-byte JSON identity.

[`mutations.json`](mutations.json) records a pristine control and **10/10 killed
mutations**. The checks cover promotion, negative evidence, disable, target
matching, expiry, schema/private-directory validation, merge, final flush and
pending-delta retention. A surviving unknown-schema mutation exposed a missing
fixture; the added valid-but-extra-field case now kills it. Mutations compile
isolated source copies and never alter the working tree.

## Check status

| Check | Result |
| --- | --- |
| Default CLI/TUI integration | 7 tests passed, with both wires and all three tool profiles |
| Native learner unit/process tests | Passed, including deferred flush and provenance checks |
| Targeted mutations | Pristine passed; 10/10 killed |
| Checkpoint and subagent ownership/fault gates | Passed; opt-out is retained |
| `make quality` | Passed; GCC analyzer explicitly skipped on Darwin |
| Final `make test` / `make leaks` | Running at this checkpoint; not claimed complete |

The first aggregate observed a transient const-qualification compile error while
the rejection wrapper was being added. The signature is corrected, focused
checks pass, and the final aggregate runs against the corrected sources.

The stripped macOS arm64 binary measures **1,104,176 bytes**, up 16,592 bytes from
the pre-change 1,087,584-byte binary. Dependencies remain `libc++.1.dylib` and
`libSystem.B.dylib`. This measures footprint; it is not a size improvement claim.
No Python process or dependency is introduced into default agent turns.

Wasm keeps the learner in current-turn memory through the host-storage seam.
SSH operations are not classified in this version. Nix source/test filters were
updated using existing dependencies; Nix was not available locally.
