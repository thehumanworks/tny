# Agent-first modernization: evidence ledger

Base: `165bcd60` (main after PR #201), 2026-10-19. Decisions:
[ADR 0189](../adr/0189-agent-first-modernization.md). This is a measured
increment on the existing architecture, not a rewrite or a competitor-best claim.

## Delivered seams

- Progressive code-mode discovery: search bounded summaries, inspect exact schemas
  only when relevant; the filtered authority/catalog and old API remain intact.
- Python plugin authoring: typed shared runtime/checker loader, machine-readable
  event/action catalog and diagnostics without provider startup or implicit code
  discovery. Explicit checks execute trusted import/setup, not handlers.
- Provider simplification: pure error/recovery policy extracted from `openai.c`;
  its transport/turn state no longer owns those tests or policy internals.
- Modern auth default: old stored/environment Codex credentials no longer select
  legacy transport implicitly. Explicit flags/settings/resume choices remain
  compatible. Sign-in tokens never migrate across provider boundaries.
- Persistence: interrupted learner writes no longer collide with stale PID
  temporaries; atomic publication consumes deltas once even if directory sync
  is unsupported. This is best-effort crash persistence, not power-loss proof.
- Cancellation: scheduler/worker ownership spans launch-to-PID-publication gaps,
  coordinates one TERM and shields cleanup from repeated interrupts.
- [Development map](../development-map.md) points agents to owners and focused
  tests instead of requiring broad source traversal.

## Measured context and startup

Both binaries were built locally with the same GCC 14.2 and pinned embedded
CPython 3.14.7 runtime. The baseline is an unchanged detached worktree at the
base commit; the candidate includes implementation checkpoints through
`64d6a2a2`, including the descriptor analyzer follow-up recorded below. Exact
binary hashes are in the raw reports. Local Linux x86_64 measurements, no live inference.

| Metric | Baseline | Candidate | Interpretation |
| --- | ---: | ---: | --- |
| Discover `read_file` + its identical complete schema | 20,168 bytes | 503 bytes | 97.51% less UTF-8 output for this specific discovery workflow |
| `--version` median | 2.575 ms | 2.458 ms | -0.116 ms; existing startup gate passes |
| `ask --help` median | 2.401 ms | 2.479 ms | +0.078 ms; existing startup gate passes |
| First fully painted prompt median | 2.752 ms | 2.829 ms | +0.077 ms; existing startup gate passes |

Raw reports: [discovery](agent-first-modernization/discovery.json),
[startup](agent-first-modernization/startup.json). Startup uses 102 CLI samples
and 20 PTY launches per artifact in alternating batches. This is **not** a
tokenizer benchmark, paid inference comparison, task-quality improvement, TTFT
claim, or speedup claim. Search is not a new provider/model tool call: it runs
inside the existing code cell, without printing the whole parameter catalog.

Reproduce:

```sh
python3 tests/bench/bench_discovery.py --baseline /path/to/base/tny \
  --candidate /path/to/new/tny --output /tmp/discovery.json
python3 tests/bench/bench_startup.py --baseline /path/to/base/tny \
  --candidate /path/to/new/tny --output /tmp/startup.json
```

## Verification ledger

| Check | Observed result |
| --- | --- |
| Native release and debug builds | Passed with GCC 14.2; public ABI unchanged |
| Code runtime suite | 24 tests / 166 C assertions passed under ASan+UBSan; embedded Python assertions cover all limits 1–100, schema projection, exact bytes and UTF-8 boundary cases |
| Production execution code-mode integration | 22 tests passed, one wasm-only seam skipped on native; both HTTP wires and filtered discovery exercised |
| Python extension suite | 33 tests passed, including installed-layout CLI and actual JSONL host, no implicit discovery, partial failure and package namespace reuse |
| Provider error unit checks | 11 tests / 293 assertions passed independently; combined provider suites 43 tests / 11,751 assertions passed under ASan+UBSan |
| SIWC local mock and focused config checks | Passed, including no-token-migration/no-network legacy credential default and explicit legacy compatibility |
| Learner recovery suites | 14 tests / 170 assertions passed, including partial-write failure/retry, abandoned temp preservation, concurrent merge; fault-injected directory-sync failure does not double-count |
| Shell workflow suites | Bash 5.2 and Zsh 5.9 passed, including hostile children, both PID-publication gates and repeated cancellation; ShellCheck 0.11.0 and shfmt 3.14.0 passed |
| Startup regression | All three paired existing gates passed; see raw data above |
| Full native unit suite | 662 tests / 51,310 assertions: 646 passed, 16 failed at executor-restricted ACP/runner process/socket setup; not a full-suite pass |
| Source-linked execution protocol proof | 9,600 compiled cases; seven properties proven over all 192 input bits with a satisfiable non-vacuity witness, using Clang 19 and Z3 |
| Provider HTTP protocol integration | Full local mock suite passed on the measured final artifact in 59.8 s, including deterministic FIN and genuine RST interruption on both wires |
| Formatting, strict warnings and lint | Final formatting, GCC strict warnings, Ruff, shell, workflow, AST and JavaScript gates passed; Clang tidy completed 144 C and 12 C++ files without diagnostics |
| GCC path-sensitive analysis | `make analyze` passed: 157 source files clean, including 12 C++ files and positive/negative ownership controls; no new suppressions |
| Hosted checks | Not run: branch publication awaits explicit approval |

## Verification limits and retained boundaries

- The initially missing Clang/Z3 tooling was supplied in an isolated temporary
  prefix; the production-predicate proof now passes. Hosted CI remains required
  for the full platform contract and Lean proof regeneration.
- LeakSanitizer cannot operate under this executor's ptrace. ASan/UBSan checks use
  `ASAN_OPTIONS=detect_leaks=0`; no leak-check pass is claimed.
- ACP private bridge/stdin and 15 runner fixtures encounter this executor's
  process/socket restrictions: creating an `AF_UNIX` socket returns `EPERM`.
  These remain unverified locally, rather than weakening admission or switching
  authentication to make fixtures pass.
- No macOS, wasm binary, real ChatGPT/Claude account, live-model quality or paid
  model benchmark is claimed. The verified Claude ACP adapter is retained because
  it already delegates to the official SDK/Claude Code. No parallel SDK loop or
  token extraction was added.
- Learning still stores only bounded counters/source session IDs and uses fixed
  advice templates, with existing promotion/demotion and explicit held-out
  experiments for broader instruction changes. It does not silently rewrite
  harness code, model weights, permissions or user policy.

The shell counter flake reported after PR #201 was reproduced as test-fixture
lock reentrancy: cancellation arrived while the fake worker held its own counter
lock. Post-lock readiness now makes the completion assertion meaningful. Separate
deterministic fixtures exposed and verified real publication-boundary races;
removing each new protection makes its regression fail on Bash and Zsh.

The existing HTTP reset fixture also reproduced an intermittent failure on the
unchanged baseline: reset may discard bytes that the server already wrote. The
fixture now separates deterministic truncated FIN (strict partial continuation)
from actual RST (exact original-input replay or strict received-prefix
continuation). Delayed-reader and altered-input negative tests prevent loosening
the contract; the corrected FIN fixture passed 12 baseline repetitions.

GCC 14 also reproduced a baseline false positive around an already-live RAII
owner adopting `open(...)`. The receipt path now checks `open` before immediate
RAII adoption. Full-translation-unit GCC analysis and 2,000 valid/rejected
receipt checks passed without descriptor growth or warning suppressions.
