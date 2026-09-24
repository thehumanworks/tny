# Harness efficiency benchmark

Status: in progress (2026-09-24). This page defines what "optimised" means for
tny as an agent harness, how it is measured against other harnesses, and the
baseline observed before any change. Results tables are appended by the
benchmark report (`tests/bench/harness_bench/report.py`).

## What we optimise

Primary objective: **price-weighted cost per completed task** at **no loss of
task success**. Cost is measured per task, not per request: a change that
shrinks requests but adds turns can cost more.

Two layers are measured and tuned separately.

| Layer | What it is | What tny controls | Metrics |
| --- | --- | --- | --- |
| **Harness** (tooling, environment) | Everything tny sends and executes: system prompt, tool schemas, injected context, tool result formatting, output offload, caching layout, compaction, isolation | Directly | Static prefix tokens (system / tools / injected), cache hit rate, tool-result tokens per call, context tokens per request, tool error rate, requests per task, TTFT, startup |
| **Agent** (model behaviour) | The decisions the model makes inside the harness: which tools, how much to read, when to stop, delegation | Indirectly (definitions, triggers, defaults) | Pass rate, steps to pass, output + reasoning tokens, wasted reads, cost per completed task |

Value density: every token placed in context should be likely to change a
decision. Large outputs belong in files that the agent can grep, tail, or read
in ranges, not inline in the transcript that every later request re-reads.

Rules we keep (from the reference in `docs/web-resources/`):

- Change what the harness sends, not how hard the model tries. No prompt text
  asks the model to "save tokens" or "do less".
- Do not truncate data away. Spill it to a file and return a path, size, and
  a short head/tail.
- Keep the cached prefix byte-stable; volatile values go after it.
- Never drop reasoning items to save input tokens.
- Ship a change only when cost per completed task drops and success does not
  regress beyond noise. Record null results.

## Cost model

Token classes come from provider usage fields: uncached input
(`input_tokens - cached_tokens`), cached input (`cached_tokens`), output
(`output_tokens`, including reasoning).

Cost is reported in **input-token equivalents (ITE)** with the benchmark
model's published ratios. For the gpt-6 family: uncached `1.0`, cached
`0.1`, cache write `1.25`, output `5.0`. gpt-5.6-terra/luna use output
`6.0`. Requests over 272K input tokens are billed at 2x input for the whole
request. Dollar figures use the Standard-tier list prices below.
Subscription use is converted with the same prices; it is a comparison unit,
not a bill.

| Model (USD per 1M, verified 2026-09-24) | Input | Cached | Cache write | Output |
| --- | ---: | ---: | ---: | ---: |
| gpt-6-astra | 10.00 | 1.00 | 12.50 | 50.00 |
| gpt-6-sol | 2.00 | 0.20 | 2.50 | 10.00 |
| gpt-6-luna | 0.10 | 0.01 | 0.125 | 0.50 |
| gpt-5.6-terra | 2.00 | 0.20 | 2.50 | 12.00 |
| gpt-5.6-luna | 0.20 | 0.02 | 0.25 | 1.20 |

Source: developers.openai.com/api/docs/pricing (Standard tier).

Rewriting history costs cache writes: an edit that invalidates `A` tokens
after the edit point and removes `R` tokens pays back only after about
`11.5 × A / R` later requests. Context editing must be batched and
placed where that holds.

## Baseline from real usage (before changes)

Source: all 59 saved tny sessions under `~/.tny/sessions` (2,337 requests,
mostly `gpt-6-sol`, `gpt-6-astra`, `gpt-5.6-sol`).

| Measure | Value |
| --- | ---: |
| Input tokens | 258.9 M |
| Cached input tokens | 252.0 M (**97.3%** hit rate) |
| Output tokens | 1.20 M |
| Mean input tokens per request | **110.8 K** |
| Cost share (ITE, gpt-6 ratios): cached input / uncached input / output | 66% / 18% / 16% |

Cache misses are rare. The cost driver is the size of the context that every
request re-reads. Tool results in saved transcripts (14.8 MB total):

| Tool | Bytes | Calls | Mean bytes/call |
| --- | ---: | ---: | ---: |
| terminal | 11.6 M (79%) | 1,862 | 6,247 |
| read_file | 2.1 M (14%) | 445 | 4,760 |
| web_fetch | 0.28 M | 23 | 12,197 |
| grep_files | 0.22 M | 170 | 1,306 |

Static prefix of one rendered request (`build/tny`, `openai` provider,
default `all` tool profile, empty project): system instructions 373 tokens;
**38 tool schemas = 4,639 tokens**. The largest are `job_submit` (569),
`image_contact_sheet` (484), `image_export` (445), `subagent` (325),
`team_mailbox` (264), `swarm_message` (227), `team_control` (216).

## Cross-harness benchmark

### Fairness rules

- Same model and reasoning effort for every harness in a comparison row.
  Rows with a different model are flagged and excluded from the headline.
- Every harness runs its **default** system prompt and tools, with user-level
  configuration isolated (fresh `HOME`: no user AGENTS.md, skills,
  extensions, or MCP servers).
- All model traffic goes through one local recording proxy
  (`tests/bench/harness_bench/proxy.py`) that forwards to the same upstream
  (`chatgpt.com/backend-api/codex/responses`) and records usage from the
  provider's own `response.completed` event. Harness self-reports are not
  trusted.
- Verification is hidden from the agent and runs after the harness exits.
- Verifier prerequisite failures and verification timeouts are environment
  errors. Report them separately from task failures and rerun before comparison.
- Each (harness, task) runs at least 3 times; report mean and spread.

### Evaluation rules

Adopted from the two harness-optimisation papers in `docs/web-resources/`
(Harbor, arXiv 2604.20938; AHE, arXiv 2604.25850). Both report
best-of-several results that fall inside their own noise.

- Freeze model, effort, timeout, and upstream per comparison. Only the harness
  varies.
- tny before/after decisions: ≥5 reps per task. Cross-harness table: ≥3 reps
  per (harness, task), with 95% Wilson intervals on pass rate.
- Pair by task. Success is a non-inferiority gate: ship only if the lower
  bound of the paired Δsuccess is ≥ −1 task-equivalent (δ = 8 pp at 12
  tasks), declared before the run. Cost is tested for significance with a
  paired per-task log-ITE ratio and bootstrap CIs over tasks.
- Report ITE per completed task as Σ ITE of **all** runs (failed, timed out,
  and aborted included) ÷ Σ passes. Failures count as failures.
- Never report the best of several runs. The selected configuration is
  re-run with fresh reps, and that confirmation run is what gets reported.
- Change one thing at a time, then run the full stack; the gains are not
  expected to add up. Record null results.
- Before a full run, smoke-check that every new feature actually fires
  (non-zero counters). Measure the baseline in the same session window with
  interleaved order.
- People designing harness changes never read `verify.sh` or hidden tests.
- Publish the per-task flip table (fail→pass, pass→fail), not only
  aggregates.

`tests/bench/harness_bench/report.py --compare ARM_A ARM_B --harness tny
--margin -8 --out comparison.md` applies these rules to two run directories
and writes Markdown plus JSON. It requires matching task/repetition keys,
uses 10,000 fixed-seed bootstrap resamples of whole tasks, and reports the
paired success delta, geometric mean per-task B/A cost ratios, and flips.
Repeat `--fire NAME=REGEX` to count evidence of a feature in saved request
bodies. See the benchmark README for the metric definitions and an example.

### Task format

```
tests/bench/harness_bench/tasks/<id>/
  task.json   {"id", "category", "difficulty", "prompt", "timeout_s", "tags": [...],
               optional "verify_timeout_s": seconds, default 120}
  repo/       initial workspace (copied, then git init + commit)
  setup.sh    optional, deterministic; run as bash setup.sh with cwd=workspace
              and no argument before the agent; optional $1 may name workspace
  verify.sh   run as bash verify.sh <workspace> <final_message_file> with
              cwd=task directory; exit 0 = pass and print a one-line reason;
              hidden tests live beside it and failures keep verify.log by
              <final_message_file>, outside the agent workspace
```

Prompts are short and phrased the way users write them. Tasks stress the
harness paths that dominate real cost: large command output, large files,
verbose test failures, multi-file edits, and codebase questions.
The default `tasks/` directory contains 12 scored tasks. The smoke task is in
`tests/bench/harness_bench/tasks-smoke/` and requires `--tasks-dir`; it is
never included by default `--task all`.

The optional `tests/bench/harness_bench/tasks-long/` suite has three longer
tasks: a 20-regression Python triage suite, a 30-client versioned API migration,
and an incident investigation whose setup generates over 90 MB of logs and
metrics. Each task has a 2,400-second agent timeout and a 300-second verifier
timeout. Its `setup.sh` runs with the
workspace as cwd and no arguments; `verify.sh` accepts the workspace and final
message paths and works from any cwd. The hidden oracle and reference solution
stay outside `repo/`.

Validate both the untouched and reference workspaces offline with:

```sh
python tests/bench/harness_bench/validate_tasks.py \
  tests/bench/harness_bench/tasks-long
```

To run live evaluations separately from the short tasks, pass
`--tasks-dir tests/bench/harness_bench/tasks-long` to `run.py`. Long runs need
a fresh output label. The offline validator does not make model requests.

### Per-run record

pass/fail, wall seconds, model requests, tool calls, tool errors, input /
cached / output / reasoning tokens, ITE cost, static prefix tokens (first
request: instructions + tools), peak and mean context tokens per request,
tool-result tokens, and the upstream HTTP status of every request.

### Headline table

| Harness | Model | Pass rate | ITE per task | ITE per passed task | Requests/task | Cache hit | Mean context tokens | Static prefix | p50 wall |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |

Filled by `report.py` for baseline and final runs.
