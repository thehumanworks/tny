# Default self-improvement and bounded experiments

**Automatic workflow learning is on by default.** Ordinary `tny ask` and TUI
work collect execution feedback and apply eligible guidance without a preset,
manual promotion, Python helper or additional provider call.

## What improves automatically

The first native mechanism learns a narrow recovery procedure: inspect current
file content before retrying an exact edit. A failed exact edit, a successful
read of the same target, and a related successful retry supply positive evidence.
After two successful episodes, the harness adds bounded empirical advice to later
normal requests and sessions. A failed retry supplies negative evidence and can
remove previously eligible advice. A cold workspace has no promoted advice.

This is experience-driven policy learning, not model-weight training, automatic
source-code rewriting, or an open-ended research loop. It currently improves
exact-edit recovery, not every kind of reasoning or tool failure. Temporal
association is not causal proof.

The learner uses typed executor facts, not assistant claims or output text.
It recognizes `read_file` and conservative foreground terminal reads (`cat FILE`
and `sed -n RANGEp FILE`). Intercepted `tny edit` works in terminal profiles too.
A different target or intended replacement, unrelated commands, failed/empty
reads, denied/nonexecuted calls and background launch acknowledgements cannot
establish recovery. User instructions and permissions remain authoritative.

Evidence lives in private `~/.tny/learning/<workspace-hash>.json` files. Only
bounded counters and source session IDs persist—no filenames, command text,
file contents or credentials. Eligible advice uses fixed templates, at most
1 KiB. Files are private, atomically replaced and merged under a nonblocking
lock. Pending updates retry at normal turn completion and carry into the next
turn of the same live backend. If the process exits/restarts while the store is
still unavailable, unflushed counters can be lost; learning must not block the
task. Corrupt state stays untouched. Original session logs remain the detailed
execution evidence; the learner's counters are not an immutable audit archive.

Inspect or disable the effective default:

```sh
tny doctor --json                 # self_improve: true by default
tny --no-self-improve ask "..."   # explicit opt-out for this process
TNY_SELF_IMPROVE=0 tny            # opt out through the environment
```

Alternatively set `"self_improve": false` in `~/.tny/settings.json`. The environment
overrides settings; `--no-self-improve` overrides both. Disabling does not delete
existing evidence. With active turns stopped, remove the workspace's learning
JSON to reset it; the next ordinary turn starts cold.

Normal native CLI/TUI turns persist learning. `--ephemeral`, libtny and wasm keep
only current-turn memory and do not load ambient learning files. SSH operations
are not classified yet and do not write local learner state. Prompt optimisation
and standalone tools do not run this loop. Help/version do not load learner
state. Opt-out follows detached runners, subagents, nested terminal commands and
job launches.

See [ADR 0154](adr/0154-default-automatic-workflow-learning.md) and the
[automatic-learning evidence](verification/automatic-learning/README.md).

## Optional: broader instruction experiments

This opt-in workflow evolves a **task instruction body**, not tny's executable,
permissions or model weights. Each accepted revision becomes the next parent.
Fixed external checks, a finite candidate budget, archived evidence and separate
promotion bound the loop. It is an element of recursive self-improvement, not
open-ended autonomous self-modification.

The [research and decision](adr/0153-bounded-instruction-evolution.md) draw on
SoL-Pi, GEPA, Darwin Gödel Machine and ACE. No result from those systems is a tny
benchmark result.

## Quick start: no provider account needed

From a source checkout:

```sh
python3 tests/bench/bench_instruction_improvement.py --out /tmp/instruction-benchmark.json
```

This runs a deterministic file-discovery workload, not a language model. It
performs real file reads and verifies exact answers. The scripted proposer and
instruction interpreter expose the feedback, selection, rejection and
parent-to-child loop without paid inference. See the benchmark's JSON and
[verification record](verification/instruction-improvement/README.md) for data
and limitations. Repeating it is a reproducibility check, not an independent
statistical sample.

For a real task, first supply a trusted independent evaluator and fixed cases.
The `self-improve` built-in preset helps an agent design that experiment:

```sh
tny --task self-improve ask "Design a bounded experiment for our review preset"
```

Selecting this preset does not automatically start a search, authorize provider
spend, or activate a candidate.

## Run a configured experiment

`make install` and release packages include two optional Python 3.9+ modules:

```text
<PREFIX>/lib/tny/tny_improve.py
<PREFIX>/lib/tny/tny_improve_propose.py
```

In a checkout, use `python/` instead. There is no `tny improve` native command,
no Python startup during ordinary tny use, and no new provider implementation.

Create a **plain Markdown task body**, without frontmatter, as `baseline.md`.
Keep existing preset metadata separately; this first version promotes plain-body
presets only. A spec might be:

```json
{
  "version": 1,
  "baseline": "baseline.md",
  "proposer": ["python3", "propose.py"],
  "evaluator": ["python3", "evaluate.py"],
  "cases": {
    "train": [{"id": "train-1", "input": {"task": "development case"}}],
    "validation": [{"id": "validation-1", "input": {"task": "selection case"}}],
    "test": [{"id": "test-1", "input": {"task": "unseen final case"}}]
  },
  "rounds": 3,
  "timeout_s": 30,
  "cost_unit": "file_reads"
}
```

Each split must be nonempty. IDs must be unique across all splits. Inputs are
arbitrary JSON for your evaluator. Use more than one case in useful experiments;
the example only illustrates the protocol. The spec and baseline are bounded
files, not an automatically discovered project configuration.

```sh
python3 python/tny_improve.py run --spec experiment.json --out "$HOME/tny-improvement-run"
```

The output directory must not already exist. It is created with mode 0700.
Commands run as argv arrays, without a shell, in the spec directory. They inherit
the environment. Relative baseline and command file paths are relative to that
directory. Pin absolute executables and evaluator source revisions for published
experiments. The spec/argv are hashed, but hashing an argv does not pin the code
behind `python3`, a script or a mutable external service.

## Command protocol

Both commands receive one JSON object on stdin and return one JSON object on
stdout. Send diagnostics to stderr. Nonzero exit, timeout, malformed JSON,
duplicate keys, nonfinite cost or oversized output fails the run closed. Input,
stdout and stderr are each bounded to 1 MiB. Task bodies are nonempty UTF-8,
NUL-free, at most 64 KiB, without frontmatter. The round limit is 1–20; each
command's timeout must be positive and at most 3600 seconds. Failure or interrupt
allows up to 20 seconds for graceful cancellation before a forced process-group
stop, then up to 5 seconds to collect terminal output. Cleanup observes inherited
pipe closure, not just the wrapper's exit, and defers interrupts until command
evidence is written. Commands receive `TNY_IMPROVE_TIMEOUT_S` with their configured
deadline. Trusted wrappers must retain inherited output pipes during cleanup;
independently detached/redirected descendants need their own cancellation logic.

The proposer receives only:

```json
{
  "parent_instructions": "Current accepted task body",
  "round": 1,
  "training_feedback": [
    {"id": "train-1", "passed": false, "cost": 3, "feedback": "Independent failure detail"}
  ]
}
```

It returns `{"instructions":"Revised task body","rationale":"Why try this"}`.
Its rationale is not evidence. Validation/test cases and results are not sent
through this protocol. This is a data-flow boundary, **not a filesystem sandbox**.
A command with access to the spec or corpus can read it; isolate real proposer
workloads if held-out secrecy matters.

The evaluator receives:

```json
{"instructions": "Candidate body", "case": {"id": "train-1", "input": {}}}
```

It returns exactly `{"passed":true,"cost":3,"feedback":"Verified detail"}`.
`passed` must be a Boolean. `cost` must be a finite nonnegative number, not a
Boolean. The evaluator must check actual task results. Do not parse success from
assistant prose. Keep provider/model/effort, tools and task limits fixed across
candidates. Evaluate instructions in fresh sessions or disposable workspaces;
never resume a session with a different preset snapshot.

### Selection and final evaluation

1. Evaluate baseline training and validation cases.
2. Generate a candidate from the current parent and its training feedback.
3. Evaluate that candidate on the same training and validation cases.
4. Reject any per-case pass-to-fail regression or aggregate cost increase in
   either split. Require strictly more training passes or lower training cost.
   Ties and identical bodies are rejected.
5. Retain the accepted child as the next parent; retain rejected evidence too.
6. After all rounds, evaluate the original baseline and frozen winner on test
   cases. These results do not feed proposal generation or candidate selection.

The controller does not equate “eligible” with a universal improvement. Final
test results are an audit, not another adaptive selection round. **Do not promote
if that audit regresses.** Investigate on new development data and reserve a new
holdout before another search. Validation can overfit after repeated selection.
Independent evaluation is only as good as its oracle and corpus.

## Native model proposals: explicit live authorization

After authorizing provider use, replace the proposer argv with, for example:

```json
["python3", "/absolute/prefix/lib/tny/tny_improve_propose.py",
 "--tny", "/absolute/prefix/bin/tny",
 "--provider", "codex", "--model", "YOUR_MODEL", "--timeout", "300"]
```

Set the outer `timeout_s` to at least **400** for this example. The adapter refuses
to launch unless the outer budget leaves at least 75 seconds beyond `--timeout`
for launch, observation and cancellation. The
adapter selects a fresh private workspace, disables extensions, sets permission
mode `ask`, caps native model steps (default 4), and uses `ask -B --json` plus
`session --wait --json`. It requires a successful settled session, not just a
successful launch. A failed wait or interrupt requests cancellation and observes
settlement; it does not resubmit. If launch output is lost, the adapter recovers
the session identity from its private workspace. It records the workspace before
launch and retains partial launch output on failure. Unconfirmed cleanup is
reported, never called success.

Its stderr records the session identity and workspace for inspection. The private
workspace stays on disk so `tny --cwd PATH session ID` remains usable. Remove it
only after inspecting the archived run and confirming settlement. Abrupt SIGKILL,
host failure, or failure of the CLI itself can still require manual recovery.
These controls do not sandbox a trusted tny process or hide host credentials.
The adapter itself requests no tools and sends no evaluation case files.

For reproducible live measurements, retain provider/model/effort, session usage,
evaluator version, dataset revision, all attempts and total search spend.
The controller's `cost_unit` is whatever the trusted evaluator measures; it is
**not automatically API cost**. The replay benchmark records all logical workload-read operations and their
returned bytes, but excludes oracle reads, setup, hashing, Python startup and
controller/proposer overhead. It cannot account for live inference dollars. Pair baseline and winner,
repeat independent runs, and report uncertainty before claiming live gains.

## Inspect, promote, and roll back

The run keeps baseline/final Markdown, the frozen spec and inputs, command
requests/stdout/stderr/status, each decision, a JSON report and SHA-256 manifest.
Failed runs keep failure evidence and are not promotable. Promotion replays the
archived decisions without running proposer/evaluator commands again, checks
hashes, and requires an unchanged existing baseline target:

```sh
# Review report.json, raw evidence, final.md and the held-out comparison first.
python3 python/tny_improve.py promote --run "$HOME/tny-improvement-run" \
  --target .tny/tasks/review-local.md --expected-sha256 BASELINE_SHA256
```

The expected SHA-256 comes from the reviewed report and must match the target's
exact current bytes. Symlink paths, stale targets and incomplete/tampered
archives are refused. Use canonical paths without symlink components (macOS
`/tmp` and `/var` are symlinks). Targets inside the archive are refused even
through `..` aliases. The replacement is atomic and preserves file mode.
Serialize other writers: portable atomic replace is not filesystem compare-and-
swap against a concurrent writer. No run changes a task file automatically.

`baseline.md` is the rollback copy. After inspecting the current target, restore
it using your normal reviewed file update or version control. Start a **fresh**
tny session after promotion or rollback; resumed sessions retain their immutable
original task snapshots. Project/user/workflow preset precedence is unchanged.

## Trust and platform limits

- Commands and the run directory are trusted operator inputs, not model output.
  They run with your user permissions. This is not an adversarial code sandbox.
- A candidate cannot change the configured evaluator, cases or authority through
  the JSON protocol. A malicious process with filesystem access still can.
- Hashes catch corruption, not forged evidence plus rewritten hashes. Do not
  promote an archive received from an untrusted party.
- Raw feedback can contain private task data. Keep archives private, do not
  commit secrets, and review data before sending it to a provider.
- Native local POSIX Python is supported. No SSH workspace or remote promotion.
  wasm has no subprocess/Python workflow; the documentation-only preset can be
  selected, but does not make the workflow available in the browser. No C ABI
  or automatic extension hook is added.
