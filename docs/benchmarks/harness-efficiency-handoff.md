# Harness optimisation: handoff (paused 2026-09-25)

Work paused overnight. This page is the entry point for resuming it. The
method, cost model and real-usage baseline are in
[harness-efficiency.md](harness-efficiency.md). Every experiment result is in
[harness-efficiency-ledger.md](harness-efficiency-ledger.md).

## The goal (user's request, verbatim)

> goal: optimise cost efficiency and performance (output quality) of 'tny' as
> an agent harness.
> brief guidance:
> - clearly separate optimising the agent / llm from optimising the harness
>   (the tooling, environment)
> - define what optimising means, it's a mix of maximising value density -
>   both in input and output, making tool calls efficient, making good
>   decisions, preventing very big file reads and rather saving the output to
>   a file, getting an agent to extract the important content...
>
> critical:
> - benchmark! get the data on how we perform now. The bottlenecks, the
>   things 'tny' excels in as a harness and the ones we are behind in
>   comparison to others.
> - research: do competitor analysis, investigate how other harnesses do it -
>   github.com/openai/codex, oh-my-pi, earendil-works/pi, hermes,
>   unreal-agent, fx.sh...
> - iterate: use subagents in parallel to explore different ideas, run in
>   separate worktrees, validate the results, merge in only the successful
>   experiments.
> - benchmark & report: confirm the optimisation results in a easy to read
>   table against the most effective agent harnesses right now.
>
> using herdr, create multiple panes in this project, run multiple tny agents
> across separate worktrees, monitor their output and work and steer them
> until the job is done and you can make a claim that tny is the most cost
> efficient, highest performing harness. For model choice, pick a mix of
> 'gpt-6-sol' from agent harness 'codex' (--yolo mode, creating worktree with
> --worktree) with effort 'high,xhigh or max' depending on task complexity
> and native subagents with 'opus' with effort set to 'medium' or 'xhigh'

The final claim must be honest: publish every row, including the ones where
tny loses.

## Where it stands

All branches are pushed to `origin`. `main` already has the benchmark suite
(`tests/bench/harness_bench/`, merged from `opt/bench` and `opt/tasks`).

| Branch | Commit | What it does | State |
| --- | --- | --- | --- |
| `opt/final` | this commit | Integration branch: main + toolerr + delegation + compact (which contains ctxedit) | Not yet gated; defaults not flipped |
| `opt/results` | 5afb617 | Spill large tool output to a file, returning a preview and a read handle (`TNY_EXP_SPILL=1`, ADR 0171) | Review fixes done; **re-review, then merge** |
| `opt/prefix` | 0c88b06 | Static prefix 6,109 → 1,379 tokens: rarely used tools load through `tool_search` (`TNY_EXP_PREFIX=1`, ADR 0174) | Review fixes done; **re-review, then merge** |
| `opt/toolerr` | a18885b | Search tools stop failing on no-match; worktree and brace-glob fixes (no flag, ADR 0176) | Merged into `opt/final` |
| `opt/delegation` | 61ae473 | Removes the "delegate when worthwhile" system-prompt line (ADR 0177) | Merged into `opt/final` |
| `opt/compact` | 0330457 | Token-threshold compaction that keeps the cache (`TNY_EXP_COMPACT=1`, `TNY_EXP_COMPACT_TOKENS`, ADR 0175) | Merged into `opt/final` |
| `opt/ctxedit` | c697057 | Clears old tool results mid-turn (`TNY_EXP_CTX_EDIT=1`, ADR 0173) | Inside `opt/compact`; stays default off |
| `opt/verbosity` | 1562f0f | Null-result experiment flags (verbosity, batch hints, reasoning context) | Keep for reproducibility; **do not merge** |

`opt/integration` is superseded by `opt/final`.

## Results so far

gpt-6-sol, effort medium. The cost ratio is treatment cost divided by
baseline cost, paired by task, with a 95% bootstrap interval; below 1 is
cheaper.

| Change | Suite | Cost ratio | Passed | Decision |
| --- | --- | --- | --- | --- |
| Remove delegation line | long 3×3 | **0.550** [0.41, 0.64] | 9/9 both | Ship |
| Smaller static prefix | short 12×3 | **0.856** [0.765, 0.957] | 36/36 both | Ship |
| Compaction at 48K, cache kept | sessions 2×2 | ≈0.85 | 4/4 | Ship at 48K |
| Compaction at 128K, cache kept | sessions 2×2 | 0.992 | 4/4 both | Cache fixed, cost neutral |
| Spill large output | short 12×3 | 0.894 [0.854, 0.931] | 36/36 | Ship; gain partly provider noise, confirm |
| Mid-turn result clearing | long, stress | never fired | — | Ship off |
| Search no-match fixes | real-session replay | grep errors 34 → 0, glob 16 → 0 | — | Ship |
| Low verbosity, batch hints, tool profiles, reasoning context | short | null or worse | — | Dropped |

Baseline USD per task, before any change:

| Harness | 12 short tasks | 3 long tasks |
| --- | ---: | ---: |
| unreal-agent | 0.035 | 0.056 |
| pi | 0.041 | 0.086 |
| tny | 0.068 | 0.175 |
| omp | 0.070 | — |
| codex | 0.076 | 0.154 |
| fx | 0.099 | — |
| hermes | 0.320 | — |

With the delegation change alone, tny's long-task cost is 0.101. tny still
trails unreal-agent and pi.

## Next steps, in order

1. **Re-review `opt/results` and `opt/prefix`.** Have an Opus agent check the
   fixes against the earlier findings in
   `/home/tomas/.cache/tny-opt/reviews/{results,prefix}.md`. Merge each into
   `opt/final` when clean. `docs/adr/README.md` will conflict; keep all lines.
2. **Flip the defaults on `opt/final`.** Turn on prefix, compaction (48K
   trigger) and spill, each disabled by setting its flag to `0`. ctxedit stays
   off. Set ADRs 0171 and 0173–0177 to accepted (0173 stays opt-in), add the
   missing README index entries for 0171, 0173, 0174 and 0175, and update
   the user-facing docs.
3. **Run the gates** with `PATH=/usr/bin:$PATH` and the `TMPDIR` below.
   `make -j16 release`, `make test`, `make quality`. Compare against main's
   known failures (below). Record the stripped size with `wc -c`. The
   baseline is 1,180,400 bytes.
4. **Run the confirmation runs.** Run the final binary and `bin/tny-baseline`
   **at the same time** (interleaved), with tny's default retries:
   - short suite (`tasks/`), 5 reps
   - long suite (`tasks-long/`), 3 reps
   - session suite (`tasks-session/`), 2–3 reps
5. **Run the headline.** Run the 6 held-out tasks (`tasks-heldout/`, never
   used for tuning) × 3 reps. Use tny-final, tny-baseline, codex, pi, omp,
   unreal-agent, fx and hermes, all in the same time window. Mind the Codex
   weekly quota: 71% was left when work paused.
6. **Write up.**
   - Update `harness-efficiency.md` with the confirmed results, the null
     results and the gaps.
   - Publish the HTML report as an artifact, with harness and agent layers
     shown separately.
   - Merge `opt/final` into `main` after the gates pass.
   - Make the claim only as far as the held-out numbers support it.
7. **Next experiments.** After 1–6, follow
   [harness-efficiency-ideas.md](harness-efficiency-ideas.md): a ranked,
   data-mined list (freeform patch tool, freeform terminal, verify-in-step,
   cheap-model extraction), each measured against `tny-final`. Tracked in
   [issue #194](https://github.com/thehumanworks/tny/issues/194), which also
   requires Lean 4 proofs bound to the C code by golden vectors, as
   `gui/proofs/` does (ADR 0169).

## How to run things

All heavy work lives under `/home/tomas/.cache/tny-opt/` (the "work root").
`/tmp` is a small per-user tmpfs that filled once and broke every shell.

```sh
export TMPDIR=/home/tomas/.cache/tny-opt/tmp
export PATH=/usr/bin:$PATH      # mise's python3 shim breaks integration fixtures
```

**Benchmark arm pair.** This is the interleaved short-suite example; both
arms run concurrently from a frozen checkout of the bench code.

```sh
B=/home/tomas/.cache/tny-opt/bin
cd /home/tomas/.cache/tny-opt/bench-frozen
R=/home/tomas/.cache/tny-opt/runs/arms
TASKS=$(ls tests/bench/harness_bench/tasks | grep -v smoke-hello | sed 's/^/--task /' | tr '\n' ' ')
L="--harness tny $TASKS --reps 3 --concurrency 4 --model gpt-6-sol --effort medium"
( uvx --with zstandard --with tiktoken python tests/bench/harness_bench/run.py \
    $L --tny-bin $B/tny-baseline --label base2 --out $R > $R/base2.log 2>&1 ) &
( uvx --with zstandard --with tiktoken python tests/bench/harness_bench/run.py \
    $L --tny-bin $B/tny-arm-prefix --label prefix --out $R > $R/prefix.log 2>&1 ) &
wait
```

**Comparison.** Pair by task, bootstrap over tasks, −8 pp non-inferiority
margin:

```sh
uvx --with tiktoken python tests/bench/harness_bench/report.py \
  --compare $R/base2 $R/prefix --harness tny --out $R/cmp-prefix.md
```

**Other harnesses:** add `--harness codex --harness pi …` to one `run.py`
call. **Re-grading after an oracle fix:**
`rescore.py <run dir> --tasks-dir <suite>`. Long and session suites use
`--tasks-dir tests/bench/harness_bench/tasks-long` or `tasks-session`.
Session resume works for tny and codex only.

**Binaries in `bin/`:**
- `tny-baseline` is frozen main at f90fc2d, the reference arm.
- The `tny-arm-*` wrappers set one flag and exec a copied worker binary.
- `unreal-agent-runner` drives unreal-agent.

**Frozen bench checkouts:**
- `bench-frozen` (f90fc2d): short suite.
- `bench-long` (39c9688): long suite.
- `bench-session` (0e3588f): session suite.

For the final runs, use a checkout of main at or after 0dae0a7, so the
held-out tasks and tny's default retries are both in.

## Work root map

| Path | Contents |
| --- | --- |
| `ledger.md` | Experiment ledger; a copy is committed here as `harness-efficiency-ledger.md` |
| `briefs/` | Worker briefs; `_common.md` holds the shared rules |
| `status/*.json` | Worker reports: branch, commit, checks, mock measurements |
| `reviews/*.md` | Opus code reviews per branch |
| `research/{codex,pi-omp,hermes-unreal-fx,papers}.md` | Competitor and paper research, plus the clones |
| `research/tasks-audit.md` | Oracle audit. **Keep it away from harness designers**; it describes hidden checks |
| `runs/{matrix,arms,long,session}/` | Every saved run, with `cmp-*.md` comparisons (26 GB) |
| `wt-final/` | `opt/final` worktree |

## herdr workers

herdr workspace `w3`, tab `w3:t5`, plus toolerr in `w3:t6`. These are codex
agents, gpt-6-sol, all idle:

| Worker | Pane |
| --- | --- |
| bench | p5 |
| tasks | p6 |
| results | p7 |
| ctxedit | p8 |
| prefix | p9 |
| compact | pA |
| toolerr | pB |

Most have little context left. Start fresh workers for new tasks, for
example:

```sh
herdr agent start NAME --kind codex --pane ID -- --yolo --worktree -m gpt-6-sol \
  -c 'model_reasoning_effort="high"'
```

Then point them at a brief in `briefs/`.

## Gotchas

- **Runner flags.** Every native turn runs in a detached session runner that
  rebuilds `tny_ctx` from the checkpoint start packet. A flag parsed into
  `tny_ctx` in `config.c` must also be carried in that packet, or it is
  silently off. `getenv` flags reach the runner.
- **Provider drift.** Cache-miss rates move by hours. Only compare arms that
  ran concurrently.
- **Check that features fired.** `report.py --fire NAME=REGEX` counts
  matches in the saved request bodies.
- **Main's known gate failures on this host:**
  - `make quality` stops at lint-sh, because zsh is not installed.
  - GCC `-fanalyzer` reports 9 existing diagnostics in `jobs.cpp`,
    `ownership.hpp` and `resources.hpp`.
  - valgrind is not available.
  - `make test` is clean with `PATH=/usr/bin:$PATH`.
  - An 18 s controller timeout and a swarm `owner.lock` race have flaked
    under parallel load; they pass on rerun.
- **Credentials.** The recording proxy injects the real token from
  `~/.codex/auth.json`, and harnesses get fake credentials in isolated
  HOMEs. Never print or copy that file.
- **Stale worktrees.** Leave these alone; they are not part of this work:
  `~/.tb.*`, `~/.tm.*`, `/tmp/tny-openai-only-*` and
  `~/.codex/worktrees/3e4a`.
