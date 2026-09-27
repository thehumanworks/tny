# Python code-mode migration scratchpad

Date: 2026-09-27
Branch: `feat/python-code-mode-runtime-20260927`
Parent: `175092fdac88825b65ad10f8f9b0f1ddc630e177`

## Goal

Migrate Tny's embedded code-mode agent runtime from Lua to Python. Benchmark
lighter Python-compatible runtimes first; prefer a lighter runtime only if it
does not reduce the already measured Python code-generation efficiency. Binary
growth is acceptable when justified by the agent-efficiency benefit.

## Required process

- Work only in this worktree.
- Preserve the prior Lua/QuickJS/CPython benchmark as immutable evidence.
- Keep benchmark protocol/results and runtime-selection rationale reproducible.
- Use real `gpt-6-luna` Codex-account trials when comparing prompt/runtime
  compatibility; do not infer generation efficiency from popularity.
- Formally verify changed deterministic policy/runtime gates with Lean.
- Run production regressions and explicit migration tests.
- Commit frequently; do not force-push.
- Claude Opus is the implementation/research agent. Its durable transcript is
  written outside Git under `.agent/`.

## Candidate runtime questions

At minimum investigate CPython, MicroPython, PyPy, RustPython, and other credible
maintained embeddable Python runtimes. A candidate must run the Python benchmark
task corpus without rewriting generated programs. If its Python subset/API
requires model prompt changes, run paired Luna generation trials against that
actual runtime before claiming equal efficiency.

## Initial evidence inherited from PR #197

CPython generation: 36/36 first-pass, 126.25 output tokens/solved task.
Lua generation: 35/36 first-pass, 172.81 output tokens/solved task.
Installed CPython probe: 31,867,976-byte stripped shared runtime plus host/stdlib.
Lua probe: 277,248 bytes.
Python passed the preregistered model-efficiency threshold; migration was rejected
only on footprint/containment trade-offs. User has now explicitly accepted those
costs if lighter alternatives lose efficiency.

## Journal

### 2026-09-27 — worktree created
Migration starts from the completed/reconciled benchmark revision. No runtime
selection has been made yet.
