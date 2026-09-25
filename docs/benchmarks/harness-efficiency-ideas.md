# Harness optimisation: next investments (2026-09-25)

What to build next, chosen from the recorded benchmark runs rather than from
intuition. Read [harness-efficiency-handoff.md](harness-efficiency-handoff.md)
first; this page assumes its results and runs after its steps 1–5.

Method: every number below comes from inference-free mining of the saved
tuning-suite recordings (`runs/arms/{base2,prefix,spill}`, `runs/long`,
`runs/matrix/baseline`) with the scripts in
`/home/tomas/.cache/tny-opt/research/mining/` (`mine.py` … `mine4.py`,
outputs `mining-v*.md`). Only the last request body of each run is read, so
each tool output is counted once. No held-out task was touched.

## 1. Where the cost is now

tny after the prefix change, short suite, gpt-6-sol medium, 36 runs, 8.2 tool
steps per run, 27.0K ITE per run:

| Component | Share of ITE | Per run |
| --- | ---: | ---: |
| Cached input (×0.1) | 24% | 6.6K |
| Uncached input (×1) | 41% | 11.2K |
| Output (×5) | 34% | 9.2K (1,842 tokens, 21% of them reasoning) |

Uncached input is the largest bucket. It is not cache misses:

| Uncached tokens by request class | Requests/run | Tokens/run | Share |
| --- | ---: | ---: | ---: |
| First request | 1.0 | 1,750 | 16% |
| Cache hit, fresh delta | 7.9 | 7,423 | 66% |
| Partial miss (cached < 50%) | 0.3 | 2,021 | 18% |
| Full miss | 0.0 | 0 | 0% |

The fresh delta on a hit request (median 603, mean 941 tokens) is:

| New items since the last request | Tokens/run | Share |
| --- | ---: | ---: |
| Tool outputs | 1,997 | 56% |
| Function-call arguments | 1,138 | 32% |
| Reasoning tokens of the previous response | 368 | 10% |
| Assistant text | 54 | 2% |
| Unexplained (provider rounding and warm-up) | ≈300/request median | — |

The unexplained residue is a provider constant: the request prefix is
byte-identical between consecutive requests in 585 of 585 pairs checked
(tny, pi and unreal-agent alike), and pi and unreal show the same ≈200–300
tokens per request. It is not a tny lever.

Visible output (function-call arguments plus final text), chars per run:

| Field | Chars/run | Share |
| --- | ---: | ---: |
| `write_file.content` | 2,055 | 46% |
| final message | 814 | 18% |
| `edit_file.new_string` | 665 | 15% |
| `terminal.command` | 544 | 12% |
| `edit_file.old_string` | 264 | 6% |
| everything else | ≈100 | 3% |

JSON encoding of arguments costs 23% more tokens than the raw values on the
short suite (1,138 vs 924 tokens per run) and 17% on the long suite.

Step structure, short suite:

| Measure | tny (prefix) | pi | unreal-agent |
| --- | ---: | ---: | ---: |
| Tool steps per run | 8.2 | 6.7 | 4.7 |
| Edit-only steps per run | 2.19 | 1.67 | 0 (edits via shell) |
| … directly following another edit-only step | 1.31 (59%) | 0.69 (42%) | — |
| Requests after the last edit, incl. final answer | 2.7 | 2.1 | — |
| Top step bigram | edit→edit (47) | edit→sh (32) | sh→sh (132) |

The worst case shows the pattern: `feature-ledger` rep 1 took tny 32
requests, 18 of them single `edit_file` calls to the same file, each in its
own step; pi took 12 and unreal-agent 6. Every extra step re-pays the cached
context (≈800 ITE after the prefix change), one reasoning burst, and the
provider residue: roughly 1.5K ITE, or 5% of a short task.

Long suite (no-delegation arm, 9 runs, 50.5K ITE per run): terminal output is
24.4K chars (≈6K tokens) per run, 58% of visible output is
`terminal.command`, and edit-only steps are 1.44 per run (54% collapsible).

## 2. Ideas the data killed

| Idea | Evidence | Verdict |
| --- | --- | --- |
| Delta reads / re-read dedupe | Second reads of a file already in context: 0.03–0.14 per run, 10–207 tokens per run | Dead |
| Anchored edits (avoid echoing `old_string`) | `old_string` is 30–66 tokens per run, 3–6% of argument chars | Dead |
| Fuzzy edit matching to avoid retries | `edit_file` failures: 0 of 136 calls | Dead |
| Dropping encrypted reasoning items | Billed at ≈0.6 × the original reasoning tokens plus 0.04 per char; ≈7% of context, cached | Not worth the quality risk |
| Fixing cache misses | Miss rate ≈4% of requests, prefix byte-stable, residue matches other harnesses | Provider-side |
| Fewer requests as a goal in itself | Unreal-style "go wider" wording: requests ×0.92 but ITE ×0.99 (N=36). A step only costs what it re-reads. | Only through mechanisms that also remove tokens |

## 3. Ranked investments

Each card: mechanism, the number that motivates it, the prediction, and the
kill rule. Decision rule for all: paired by task, 10,000 task-bootstrap
resamples, seed 20260924, arms run interleaved against **`tny-final`** (the
post-flip build, not `tny-baseline`, so gains are measured on top of what is
shipped), non-inferiority margin −8 pp on pass rate, and a `--fire` check
that the feature actually fired. Cache risk is zero for every card: each is
append-only or lives in the static prefix.

### A. Freeform patch tool (custom tool type, grammar-constrained)

Replace `edit_file` and `write_file` with one `apply_patch` custom tool in
Codex's format (`*** Begin Patch` / `*** Update File:` / `@@` hunks /
`*** Add File:` / `*** End Patch`), sent as a Responses custom tool with the
Lark grammar. gpt-6 is trained on this format; Codex uses it through the same
ChatGPT backend, and our recordings show `custom_tool_call` items pass through
it. tny's serializer has no custom-tool support yet (`openai.c`), so this
also builds the infrastructure card B needs.

- Prior: 1.31 collapsible edit steps per run (59% of edit steps); 18 in the
  worst run. Multi-hunk, multi-file patches collapse them into one step.
  JSON overhead on `new_string` + `content` (61% of visible output) disappears.
  Tool definition is 252 tokens versus ≈700 for the two schemas it replaces.
- Prediction: requests −10–15%, ITE −8–12% on the short suite, −20% or more on
  edit-heavy tasks; wall time down with requests.
- Suite: short 12×3 first, then long 3×3. Fire check: `apply_patch` calls ≥
  80% of all edits; watch patch-apply failures (currently 0 for `edit_file`).
- Kill: ITE ratio CI includes 1.0, pass rate not non-inferior, or the model
  keeps issuing one hunk per step.
- Layer: harness. Build: patch parser in C11, custom-tool serialization,
  `custom_tool_call`/`_output` items, session replay. Medium-large.

### B. Freeform `terminal`

Once custom tools exist, make `terminal` freeform: raw shell text, no JSON.

- Prior: `terminal.command` is 12% of visible output short, 58% long; JSON
  overhead 17–23% of argument tokens.
- Prediction: ITE −2–4% short, −5–8% long. Zero quality risk.
- Run as a third arm alongside A (A, A+B, control) so each is attributed.
- Kill: no measurable change on the long suite.

### C. Verify in the same step

Two variants, cheapest first:

1. One line in the tool descriptions: calls in one response run in order, so
   an edit and the test that checks it can go in the same step. The earlier
   batch-hint test (requests 10.0→9.5, N=6) was underpowered; rerun at N=36.
2. An optional `then` command on the patch tool: run a command after the
   patch applies and return both results in one tool output.

- Prior: edit→sh and write→sh transitions are 44 per 36 runs (1.2 per run);
  2.7 requests follow the last edit.
- Prediction: −1 step per run, ITE −4–6%, wall −8%. Quality: breakage is seen
  one step earlier, which is the only quality lever visible in the data.
- Kill: requests ratio CI includes 1.0 for variant 1; for variant 2, fire
  rate below 30% of patches.
- Layer: harness.

### D. Cheap-model extraction of large tool output

The brief's own idea: when a tool output exceeds the spill threshold, run
gpt-6-luna over it with the task and the command, and return its extract plus
the read handle instead of the raw preview. Luna input costs 1/20 of sol's,
so reading 6K tokens costs ≈300 ITE and the main context receives ≈300
tokens instead of 6K, which are then also not re-read on every later request.

- Prior: long suite terminal output ≈6K tokens per run, ≈18% of long-task ITE
  counting the cached re-reads; short suite tool outputs 2K tokens per run.
- Prediction: long −10–15%, short ≈0. The kill risk is quality: an extract
  that drops the needed line costs a round trip through the handle.
- Suite: long 3×3 and session 2×3 only. Fire check: extraction events per
  run and handle re-reads per extraction.
- Kill: pass rate not non-inferior, or handle re-reads exceed 50% of
  extractions.
- Layer: harness feature with a model inside; report the luna cost
  separately.

### E. Backlog, not now

| Idea | Why it waits | Ceiling |
| --- | --- | --- |
| Async tool calls with cache-safe placeholders (unreal-agent) | Speed lever, not tokens; "underspecified in the Responses API" per its own authors | wall time only |
| Per-request effort schedule | Agent layer by the brief's own separation; reasoning is 21% of output | ≈7% ITE |
| Shorter final message | 18% of visible output, but AGENTS.md wants a skimmable summary; prompt-wording arms have been noisy | ≈4% ITE |
| Cross-session prefix sharing (dynamic values last in instructions) | First request is 16% of uncached; the bench cannot measure it because concurrent runs already share the prefix | ≈6% ITE |
| Per-call output budget with head/tail split | Partly in the spill branch already | small |

## 4. Quality has no headroom yet

Every harness passes every task on both suites after the oracle fixes. Any
idea justified on quality can currently only show non-inferiority. Before a
quality experiment, add a graded score with headroom, designed blind to the
harness branches: oracle sub-checks with partial credit, files changed outside
the task's scope, tests broken or removed, and a harder tier of held-out
tasks. Card C's "breakage seen earlier" is the first thing that score would
detect.

## 5. Experiment plan and budget

Order: handoff steps 1–5 (merge, flip, gates, confirmation, headline) →
A+B (one three-arm short run, then long) → C variant 1 (short) → C variant 2
(short) → D (long and session). One card per worker in its own worktree,
each behind a `TNY_EXP_*` flag until measured, merged only on a win.

Cost per arm in runs: short 12×3 = 36 runs at ≈0.05 USD-equivalent each;
long 3×3 = 9 runs at ≈0.10; session 2×3 = 6 runs at ≈0.3. The plan above is
≈5 short arms, 3 long arms and 2 session arms, about 220 runs, on top of the
≈250 runs the handoff's confirmation and headline steps need. The Codex
weekly quota was at 71% when work paused; check it before starting D.
