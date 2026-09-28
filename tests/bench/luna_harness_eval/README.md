# Tny vs Codex, GPT-6 Luna: whole-harness evaluation

Read `PROTOCOL.md` before running inference. The completed September 28 evaluation
is in `docs/benchmarks/tny-codex-luna-20260928/report.md`. This is an opt-in experiment,
not a production/CI dependency. Production source was not modified.

## Reproduction

Use Linux, Python 3.14, a native Tny executable, the matching Codex CLI, Git, a C
compiler and Make. The existing Codex CLI must be logged in with ChatGPT. The
proxy owns the real account authentication; agents receive only synthetic login
files. Do not copy actual credentials into output directories.

Create a separate worktree and run from there. Set `TMPDIR` to the generated
root's `tmp` directory, and put its virtualenv and the concrete Python 3.14 binary
directory first on PATH (rather than a HOME-sensitive version-manager shim).

```sh
python3 tests/bench/luna_harness_eval/prepare.py --tny /absolute/path/to/tny
# Read temporary_root from the printed JSON and assign it to EVAL_ROOT.
uv venv "$EVAL_ROOT/venv" --python /absolute/path/to/python3.14
uv pip install --python "$EVAL_ROOT/venv/bin/python" tiktoken==0.14.0 zstandard==0.25.0
export TMPDIR="$EVAL_ROOT/tmp"
export PATH="$EVAL_ROOT/venv/bin:/absolute/path/to/python-bin:$PATH"
python tests/bench/luna_harness_eval/validate_tasks.py
python tests/bench/luna_harness_eval/test_accounting.py
python tests/bench/luna_harness_eval/experiment.py --live --smoke
python tests/bench/luna_harness_eval/experiment.py --live
# After the main driver exits successfully, record its terminal result:
printf '0\n' > "$EVAL_ROOT/primary.exit"
python tests/bench/luna_harness_eval/cache_eval.py --live
printf '0\n' > "$EVAL_ROOT/cache.exit"
python tests/bench/luna_harness_eval/analyze_eval.py
python tests/bench/luna_harness_eval/diagnose_calls.py
python tests/bench/luna_harness_eval/analyze_cache.py
```

Run with shell `set -e` or explicit exit-code handling: never write success after
a failed driver. In the completed experiment the durable tmux wrappers wrote
exit receipts atomically after process exit. There were 40 scored attempts and
40 separate cache turns, plus two wiring smokes. No oracle-feedback retries were
allowed in the scored cohort. Keep failed runs; do not selectively replace them.

`review_quality.py` contains manual annotations for the **published original run**.
Do not apply those annotations blindly to a new run: reread its final artifacts,
raw calls/results and verification logs. The functional oracle is the primary
quality score; the final-answer rubric is supplementary.

`export_evidence.py` exports the completed run after the audits. It removes opaque
encrypted reasoning handles, auth/config/session stores, generated binaries and
unused legacy API-price estimates. Its inventory retains both original and
sanitized file hashes. The original run's wire-body hashes therefore do not equal
the normalized JSON in the sanitized archive; that is an explicit export change,
not a claim of byte-for-byte publication of private routing state.

The account endpoint, model and CLI behavior may change. Record actual versions,
request and response model identifiers, all usage fields and task fixture hashes
for each rerun. `cost.py` is inherited harness compatibility code; its dollar and
input-token-equivalent fields are **not used** by this evaluation. No dollar,
subscription-credit, or rate-limit savings can be inferred from these counters.

## Metrics and audit

`analyze_eval.py` independently resums provider JSONL usage, checks requested and
returned model/effort, reconciles both CLI usage reports, validates all raw request
hashes, and rejects an incomplete/duplicate 40-run cohort. Ratios retain failed
runs; a secondary comparison uses the sixteen pairs where both harnesses passed.
The paired bootstrap resamples task families, preserving both repetitions.

Model-facing calls are `run_code` for Tny and `exec` for the tested Codex. Both
can batch multiple internal actions. `diagnose_calls.py` reports visible wrapper
failures and source call sites, **not** syscall-level operation counts or an
exhaustive error detector. Initial intentionally failing tests are not runtime
compatibility failures.

Cache experiments use native default routing settings. A first request is not
guaranteed cold. The two harnesses receive the same complete 200-item catalog,
with a new shared nonce for each paired repetition. Five turns are either resumed
in one conversation or started fresh in the same workspace. Actual cached tokens,
cache-key hashes and affinity-header lengths—not inferred prices—are retained.

## Cleanup

Export and verify the evidence before removing the worktree. Remove only paths
created by this experiment and identified in `OWNER.json` and `STATE.json`.
Terminate any still-active owned processes first; preserve unrelated user
worktrees, session histories, credentials and temporary directories. The
completed evaluation's cleanup receipt is in the report directory.
