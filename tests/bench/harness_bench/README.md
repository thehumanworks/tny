# Cross-harness recording benchmark

This is an opt-in live benchmark. It runs the same synthetic task in a fresh Git
repository for each harness and repetition. A loopback proxy forwards Responses
requests to the ChatGPT subscription endpoint and records provider usage. The
verifier runs afterward from the task directory; the agent never sees it.
An active workspace lives under its own randomized `ws-*` directory inside
its run directory and is moved to `workspace/` after verification. A new run
clears stale artifacts for that exact harness/task/repetition before starting.
No hidden verifier files or `verify.log` exist there while the harness turn is
active. Verification begins only after the harness process exits. The agent
can still walk up the host filesystem to other runs or the benchmark source,
including hidden tests: this runner is not an OS sandbox. Keep benchmark task
sources and run directories inaccessible to untrusted agent processes when
hidden-oracle secrecy is required.

Fixture setup runs as `bash setup.sh` with the workspace as cwd and no
argument; scripts may also accept an optional workspace path for manual use.
Verification runs as `bash verify.sh WORKSPACE FINAL_MESSAGE_FILE` with the
task directory as cwd. A failed oracle keeps `verify.log` beside the final
message, outside the agent workspace. The verification timeout is
`task.json`'s `verify_timeout_s` or 120 seconds by default; the long tasks use
300 seconds. A verifier prerequisite failure or timeout is recorded as
`status: error` with an `error:` reason, separately from a failed task.

The ledger and workflow fixtures derive from the repository's earlier public
swarm cases. Treat them as task completion checks, not as blind held-out
evidence for anyone who has worked with those cases.

The proxy reads the current `tokens.access_token` and `tokens.account_id` from
`~/.codex/auth.json` for **each** request. Harnesses receive an isolated `HOME`
with fake credentials and no user skills, extensions, MCP configuration, or
user `AGENTS.md`. The proxy does not record real credentials or response
headers. Keep run directories private: gzipped request bodies contain the
synthetic workspace and conversation.

## Run

From the repository root, after building `build/tny`:

```sh
export TMPDIR=/home/tomas/.cache/tny-opt/tmp
uvx --with zstandard --with tiktoken python tests/bench/harness_bench/run.py \
  --harness tny --harness codex --harness pi --harness omp --harness hermes \
  --harness fx --harness unreal-agent \
  --task all --reps 3 --concurrency 3 --model gpt-5.6-luna --effort low \
  --tny-bin build/tny --label baseline \
  --out /home/tomas/.cache/tny-opt/runs/bench
uvx --with tiktoken python tests/bench/harness_bench/report.py \
  /home/tomas/.cache/tny-opt/runs/bench/baseline \
  --out /home/tomas/.cache/tny-opt/runs/bench/baseline-report.md
```

`--task` and `--harness` repeat. Omit `--task` to select all available tasks.
The default `tasks/` directory contains exactly the 12 scored benchmark tasks;
`--task all` selects those 12. For a wiring check, select
`--tasks-dir tests/bench/harness_bench/tasks-smoke --task smoke-hello`.
The `session-resume` smoke task has two prompts and checks the resume path.
Use `--tasks-dir tests/bench/harness_bench/tasks-long` to select the three
long-horizon tasks; validate them offline first with
`python tests/bench/harness_bench/validate_tasks.py tests/bench/harness_bench/tasks-long`.
Their setup generates large files in each copied workspace, so keep `TMPDIR`
on a disk with enough space and use a distinct run label. No live inference is
needed for validation.
Use `--tasks-dir tests/bench/harness_bench/tasks-session` for the two
12-prompt session tasks. `task.json` uses `prompts` in place of `prompt`;
the prompts run in order within one tny session or Codex thread, using the
same workspace and proxy. The verifier runs once after the last prompt.
Pi, OMP, Hermes, FX, Unreal Agent, and OpenCode are reported as skipped on
session tasks because their noninteractive resume route has not been verified.
Each request row has a one-based `turn`, and `result.json` records each
turn's requests, provider usage, cost, exit status, and wall time, plus the
number of turns completed. The report shows completed/requested turns.
These tasks reuse the regression and migration codebases with staged
developer prompts, additional generated traces, and final documentation
checks. Validate both untouched and reference solutions offline with
`python tests/bench/harness_bench/validate_tasks.py tests/bench/harness_bench/tasks-session`.
The generated traces offer substantial output for the later turns; actual
context growth depends on the agent's tool use and has not been measured on
the full tasks.
`--tasks-dir tests/bench/harness_bench/tasks-heldout` selects the held-out
set for the final confirmation run.
Each run writes `result.json`, `stdout.txt`, `stderr.txt`,
`final_message.txt`, `workspace/`, and `proxy/requests.jsonl` plus compressed
request bodies. Existing `result.json` files are skipped, so rerun the same
command after an interruption. Use a new label when the model, effort, task
fixtures, adapter, or binary changes. `--auth-file` can select a different
Codex OAuth store. The proxy accepts the four Responses POST paths listed in
`proxy.py` and a read-only Codex model-catalog GET for FX capability lookup;
other requests get 404 and their method/path/status are kept in
`proxy/rejected.jsonl` for routing diagnosis. The catalog response is forwarded
without retaining its body or headers.

`zstandard` is optional for the Python code but needed for installed Pi, which
sends zstd-compressed requests. Without it, the proxy explains the problem
and responds 415. `tiktoken` is optional; `run.py` and `report.py` label the
chars/4 estimate when it is absent. The report writes both Markdown and JSON.

## Compare two runs

This command reads saved run results and request bodies; it makes no model
requests.

```sh
python tests/bench/harness_bench/report.py \
  --compare /home/tomas/.cache/tny-opt/runs/bench/baseline \
            /home/tomas/.cache/tny-opt/runs/bench/confirmation \
  --harness tny --margin -8 --fire compact='compaction marker regex' \
  --out /home/tomas/.cache/tny-opt/runs/bench/comparison.md
```

The comparison also writes `comparison.json`. `--margin` is in percentage
points; `-8` means B may lose at most eight points of success at the lower
95% paired confidence bound. Task and repetition keys must match where both
arms provide them; a task missing a repetition index must have one run per
arm. Model and effort must be identical. When each directory contains
one harness, `--harness` can be omitted (the harness names may differ).

The report resamples whole tasks 10,000 times with a fixed seed, retaining
all repetitions for each sampled task. ITE and USD per completed task divide
the total over **every** run, including failures and timeouts, by passes.
Paired cost and resource ratios are geometric means of per-task B/A ratios.
Zero or missing costs make a log ratio unavailable; they are not replaced
with an arbitrary offset. A run with no model requests has zero provider
cost; a request lacking usage makes that run's cost unavailable.
`--fire NAME=REGEX` can be repeated; it counts
matches, requests, and runs in the saved decompressed request JSON for each
arm. A zero-pass bootstrap sample makes the upper per-completed cost bound
unbounded.
The standard report lists verification or environment errors separately and
excludes them from pass-rate and cost denominators. Paired comparison refuses
an arm containing such a run until it is rerun, so a broken verifier cannot
be counted as a task failure.

For Unreal Agent, build its Go runner into
`/home/tomas/.cache/tny-opt/bin/unreal-agent-runner` from the cloned source:

```sh
export TMPDIR=/home/tomas/.cache/tny-opt/tmp
cd /home/tomas/.cache/tny-opt/research/unreal-agent
mise exec go@latest -- go build -trimpath \
  -o /home/tomas/.cache/tny-opt/bin/unreal-agent-runner ./cmd/unreal-agent-runner
```

## Adapter status

| Harness | Status | Route |
| --- | --- | --- |
| tny | Smoke passed | `--provider codex`, default session-runner isolation, `ask --json`; `TNY_CODEX_BASE_URL` points at the proxy |
| Codex CLI | Smoke passed | `exec --json`, model provider override, HTTP/SSE, websocket support disabled |
| Pi | Smoke passed with optional zstd | Built-in `openai-codex` with isolated `models.json` and OAuth; `transport: sse` in `settings.json` |
| OMP | Smoke passed | Built-in `openai-codex` with isolated `models.yml` and OAuth; `PI_CODEX_WEBSOCKET=0`, stdin closed |
| Hermes | Smoke passed | `openai-api` Responses transport with `OPENAI_BASE_URL` pointing at the proxy; isolated config disables auxiliary title, memory nudge, and skill creation nudge calls |
| FX 0.0.10 | Smoke passed | Built-in Codex provider with isolated ChatGPT session and settings; its Responses and model-catalog E2E URLs point at the loopback proxy |
| Unreal Agent | Smoke passed | Built-in `openai-codex`; `UNREAL_HARNESS_LLM_BASE_URL` points at the loopback proxy, default prompt and tools preserved |
| OpenCode 1.18.32 | Excluded | Its `@ai-sdk/openai` Responses request reached the proxy, but the ChatGPT upstream returned HTTP 400; no comparable usage row is claimed |

All working adapters use their ordinary system prompt and tool set. Codex's
hosted web search is disabled. The other adapters have no user search
credentials in their isolated homes; the synthetic tasks do not ask for web
search. Some default local search tools remain advertised by those harnesses.
Hermes may probe model metadata paths before inference; the proxy rejects
those probes and records only Responses POSTs as model requests. FX's override
is an explicitly named E2E hook in FX 0.0.10. Its documented custom model
connections currently use Chat Completions, so the hook is needed to measure
the built-in Codex Responses route. FX's isolated settings pin effort, and its
catalog lookup must go through the proxy so the model's supported effort tiers
are known. Each adapter's recorded request is checked for the requested model
and effort.

The smoke task checks wiring and accounting. It is not evidence of comparative
success on the full task suite. Headline input-token equivalents use uncached
input `1`, cached input `0.1`, reported cache writes `1.25`, and output `5`
for GPT-6 or `6` for GPT-5.6 Terra/Luna. The report also computes Standard
list-price USD from provider usage and the model price table in
`docs/benchmarks/harness-efficiency.md` on main. Both input components double
above 272K input tokens per request. A missing cache-write field means no
separately reported write tokens; missing required provider usage leaves the
cost unavailable. No harness self-report is substituted.
