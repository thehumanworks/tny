# Matched live swarm evaluation

The user explicitly authorized live `gpt-5.6-sol` or Luna inference through their
existing Codex account. `gpt-5.6-sol` was available in both native tny and installed
Codex CLI `0.156.0-alpha.8`; this evaluation selects it with medium reasoning effort.
No provider key or account identifier belongs in public evidence.

## Task ladder and independent scoring

`tests/bench/swarm_cases.py` prepares three fresh, self-contained Python workspaces:

| Case | Increasing difficulty | Independent checks |
| --- | --- | --- |
| `slug` | One pure Unicode normalization function | Type/range validation, normalization, truncation, fallback and non-ASCII cases |
| `ledger` | Durable state, idempotency, concurrent clients and streaming CLI | Reopen, original duplicate result, failed-write rollback, simultaneous updates, CLI recovery |
| `workflow` | Validated DAG scheduling, retries and failure propagation | Exhaustive small-DAG variants against a separate tick-based oracle, ordering, input immutability, invalid inputs and CLI |

The agent sees `README.md`, implementation stubs and a protected public test. The
external oracle remains outside its workspace and runs after the harness exits.
Changing the public test fails the trial. A claimed success without passing the
oracle is a failure. Terminal status, timeout and unfinished peer cleanup are
separate recorded outcomes, not inferred from model text.

## Matched and deliberately unmatched conditions

Both arms use the same model, effort, task text, standard-library constraint and
fresh workspace/state. Codex's subagent model/effort defaults and three-peer cap
are explicitly configured; actual session model names are checked afterward.
The first repetition runs tny then Codex, and the second reverses that order.
The release-candidate executable is copied once and SHA-256 checked before every
trial so edits or rebuilds cannot silently change the measured implementation.

The tny arm selects `examples/swarm/benchmark-review.json`: the root implements,
a nested verification coordinator synthesizes two specialist peers' evidence,
and all three collaborators start. Codex has subagents enabled with the same
maximum but chooses its own participation. This measures the requested deployed
harnesses, **not** a controlled comparison of identical prompts, toolsets, safety
policies or agent counts. tny's native default permissions and Codex's explicit
workspace-write sandbox are retained. Neither task needs network or packages.

Record two paired trials per task where execution succeeds. Preserve failures
and timeouts. This small synthetic sample supports descriptive comparisons only,
not a statistical superiority claim or a prediction for real repositories.

## Accounting

Wall time includes the lead and any bounded post-lead cleanup. tny usage sums
final cumulative counters from every root/worker session once. Codex usage takes
the final cumulative token-count record from each distinct rollout once. Root-
stream-only fallback is explicitly incomplete, never treated as zero peer cost.
Input tokens include cached input; cached input and output are shown separately.
Cache hits are measured, not assumed from stable prefixes. Provider-side cache
state is uncontrolled and these runs are not a clean cold-cache experiment.

Native tool-attempt counts come from persisted assistant tool calls, including
validation failures. Executed-call counts are reported separately. Durable
mailbox records measure direct peer participation and coordinator-to-parent
messages; acknowledgements show delivery bookkeeping, not proof that a model
understood or correctly incorporated evidence. External correctness remains the
acceptance check.

## Reproduction

Run from this feature checkout, after building a frozen release binary:

```sh
make release
cp build/tny /tmp/tny-swarm-candidate
python3 tests/bench/bench_swarm.py --live \
  --tny /tmp/tny-swarm-candidate --codex "$(mise which codex)" \
  --model gpt-5.6-sol --effort medium --agents 3 \
  --swarm-file examples/swarm/benchmark-review.json \
  --cases slug ledger workflow --repetitions 2 --timeout 360 \
  --output "$HOME/.cache/tny-swarm-live-unique-run"
```

The output directory must not exist. It is private and contains temporary login
references and raw model sessions: **do not publish or archive that directory**.
Only inspect and sanitize `result.json` for committed evidence. Offline harness
checks need no account: `python3 tests/integration/test_swarm_benchmark.py`.

## Diagnostic pilots, excluded from candidate comparisons

The pilot first established telemetry and oracle behavior. Both baseline numeric
swarm and Codex solved `slug` without collaborators (57.918 s and 70.478 s). A
second pilot exercised the draft nested definition: tny passed in 110.036 s with
three collaborators, versus Codex's 46.793 s without collaborators. tny recorded
713,936 input tokens (588,928 cached), 9,139 output tokens, 9 durable messages,
8 direct peer messages and 1 nested-coordinator upward message. Codex recorded
73,547 input tokens (43,008 cached) and 1,751 output tokens.

The nested pilot had four avoidable malformed mailbox calls. Their action-specific
requirements are now stated in the tool schema and validation diagnostic. These
pilots used draft/baseline binaries and overlapped development inference, so they
are diagnostic evidence, not release-candidate timing claims. They demonstrate
that a fixed review roster imposes overhead on a trivial task; extra agents are
not automatically an optimization. Final candidate results must be reported
separately, including unsuccessful trials.
