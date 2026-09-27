# Python runtime migration benchmark

The decision is [ADR 0179](../../../docs/adr/0179-python-code-mode-cpython.md);
the [report](../../../docs/verification/python-code-mode/report.md) distinguishes
probes, unchanged-code replay, live generations and production verification.

## Production and offline gates

```sh
make release python-cell-bench
make test-code-mode-language
make verify-code-policy LEAN=/path/to/lean-4.30.0/bin/lean
python3 tests/bench/python_runtime/audit_evidence.py
```

The actual code_policy.c is translated through Clang AST; the proof target
requires Clang/GCC with UBSan and the pinned Lean. It verifies the C predicates,
Python selection rule and negative mutations. Offline evidence gates never call
a provider or install a candidate interpreter.

## Reproduce production behavior on the saved programs

The replay helper uses Linux bubblewrap, prlimit and a two-second outer deadline.
The production binary additionally starts its actual confined code-cell process.
No user workspace, network or inherited credentials are exposed to programs.

```sh
python3 tests/bench/python_runtime/replay.py \
  --runtime production --output /tmp/production-original-replay.json
python3 tests/bench/python_runtime/replay_heldout.py \
  --samples docs/verification/python-code-mode/data/heldout-samples.json \
  --executor production --arms cpython,cpython_pr197,monty \
  --require-match --output /tmp/production-heldout-replay.json
```

The second command requires equality with every recorded pass/fail outcome; it
does not demand that the original failing model programs magically succeed.
Do not overwrite the committed original model-trial evidence with replay timings.

## Candidate builds

The candidate build helper is optional and Linux-native. It requires the
hash-pinned publisher source files in an explicit directory; it never silently
downloads or substitutes a different version. Run `build.py --help` for options.
Monty's pinned Git checkout and Rust toolchain are required only for that probe.

```sh
python3 tests/bench/python_runtime/build.py --sources /path/to/pinned-sources \
  --only micropython_compat
python3 tests/bench/python_runtime/replay.py --runtime micropython_compat \
  --output /tmp/micropython-compatible-replay.json
```

micropython_compat changes only the prebound JSON facade. It never edits the
model's program or prompt. It fixes the ensure_ascii wrapper difference; it is
not a claim that MicroPython dict/string semantics become CPython's.

## Live account-backed experiment

`trials.py` has an explicit live opt-in and a controls-only mode. Check its help
before running; each output directory must be new. Use the existing Codex CLI
ChatGPT login. Do not read or copy credentials into a fixture or source file.
The protocol fixes gpt-6-luna, low effort, complete paired task/repetition arms,
three variants and at most one repair. No valid generation may use external tools.
Use new held-out families for future tuning instead of selecting favorable
retries from this published corpus. All failed first attempts and repairs belong
in both outcome and token denominators.

`analyze_trials.py` runs the independent cohort audit before calculating metrics.
`audit_evidence.py` separately checks typed effects, object order where specified,
completion/source/usage receipts and the published aggregates. The task oracles
are shared fixture definitions; the audit does not claim independent authorship
of the task corpus or statistical confidence guarantees.

## Whole-product native comparison

`measure_release.py --baseline /path/to/lua-tny --candidate /path/to/python-tny
--output result.json` records paired warm-cache startup and exact artifact hashes,
versions, source hashes and dependencies. Use the same comparison-only version
label on both binaries, and never use that override for ordinary integration tests
that validate the actual Git version. `bench_agent_cells.py` separately measures
the full native loopback-provider execution path without model inference.
The final Unicode-complete receipts are the `release-candidate-*` files; earlier
measurements remain historical rather than being silently replaced.
