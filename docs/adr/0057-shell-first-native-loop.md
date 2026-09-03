# 0057 — Shell-first native loop: one terminal tool, tny verbs as CLIs

Date: 2026-09-02
Status: accepted (direction); the A/B of issue #103 is measured and
recorded in the Measurement section below. The default in code is
unchanged (`all`); changing it is a separate decision.

## Context

The native OpenAI-compatible loop advertises 26 function tools
(`src/core/tools.c` `SCHEMA_JSON`). Host backends (Cursor bridge, Codex
app-server, ACP clients) own their tools and are unaffected.

Evidence gathered on 2026-09-02: Vercel's d0 agent went from 15+ tools to 2
(80% → 100% success, 3.5× faster, 37% fewer tokens); mini-SWE-agent is bash
only and scores >74% on SWE-bench Verified; Terminus 2 drives a single tmux
session and placed second on Terminal-Bench; CodeAct reports code actions up
to +20 points with 30% fewer steps than JSON tool calls; Anthropic's
code-execution-with-MCP post drops 150k tokens to 2k by presenting tools as
code; Anthropic's tool search raised Opus 4 from 49% to 74% by removing
schemas from context. Against a pure single tool: Anthropic's SWE-bench
agent, Claude Code, and Codex all keep an exact-match editor beside the
shell, and SWE-agent's 2024 shell-only ablation fell from 12% to 3% on
older models.

Three independent reviewers (codex gpt-5.6-sol, grok-4.6, Claude Opus via
ACP; outputs attached to epic #105 and kept in `agents/out/`, which is gitignored) converged on the shape below.

## Decision

1. **Shell-first is a vocabulary, not a single-tool purity test.** The model
   sees `terminal` plus the smallest set the evidence defends; every other
   capability is a `tny` subcommand reachable from any shell, inside tny or
   inside another harness (Claude Code, Codex, Cursor, OpenCode, CI).
2. **Tool profiles, not deletion.** `tools: all | terminal+edit | terminal`
   (setting and `TNY_TOOLS`) changes what the native loop advertises and
   accepts. No implementation is deleted until a three-arm A/B on a frozen
   task set (#103) says so; `tools_fs.c`, `tools_ssh.c`, `tools_web.c`
   remain the backends of the CLI verbs and of `--ssh`.
3. **First-party verbs.** `tny edit` (exact match, stdin payload, exit 2 on
   zero or many matches), `tny mcp call server/tool` (identity stays
   `mcp:server/tool`), `tny ask-user`, `tny image attach`, `tny memory`,
   `tny skill show`, `tny fetch`; subagents remain `tny ask -B` +
   `tny session --wait`. Payload never rides argv; `--json` output carries a
   `kind` field; exit codes are 0 ok, 1 usage/config, 2 semantic failure,
   130 interrupted.
4. **Three harness seams stay in tny:** the session socket for human
   interaction (free-text questions, image attach), the attach path for
   non-text content (ADR 0008), and the OS sandbox for authority.
   `ask-user` and `image attach` are socket-bound; with no socket they print
   one line to stderr and exit 1, never touch the TTY.
5. **In-process intercept.** When tny is the harness, a single simple
   `tny …` command inside `terminal` is dispatched in-process so
   permissions, session grants, undo, the warmed MCP client, and `--ssh`
   routing survive. Outside tny the same verbs run standalone.
6. **Security boundary.** argv0 classification is a UX accelerator, not a
   boundary. Shell profiles are offered for `yolo` (the default, ADR 0001)
   only until the permission tokeniser (#101) and the `os` sandbox (#102)
   land.
7. **Out of scope now:** wasm and libtny keep the full structured schema
   (no miniature shell behind the wasm terminal); `--ssh` is not "a prefix
   on the command" (the remote host has no tny, ADR 0022); a persistent pty
   via tnytty is a later, separately measured ADR; size is not a rationale
   (≈35 KB of a 783 KB binary).

## Consequences

- Issues: #96 edit, #97 mcp call, #98 runner control channel, #99
  intercept, #100 profile, #101 permission tokeniser, #102 sandbox, #103
  A/B; epic #105. Per-decision ADRs: 0058 runner control channel, 0059
  permission tokeniser, 0060 os sandbox, 0061 toolchain (mise, valgrind),
  0062 tool profiles, 0063 in-process intercept, 0064 CLI verb
  conventions.
- The A/B result and the chosen default are appended to this ADR when
  #103 lands.
- Every `tny` verb becomes public API with compatibility obligations to
  other harnesses; `docs/cli.md` is the contract.

## Measurement (issue #103)

Date: 2026-09-02. Provider `aiproxy`, model `grok-4.6`, effort `high`,
`--max-steps 40`, 600 s per-run timeout, **N=1 per task per arm**. Harness
`tests/bench/bench_tools.py`; task set `tests/bench/fixtures/tools/`
(26 tasks). Primary run on tny `0.3.3-30-gac0bd11`, the merge of
`feat/shell-first` that contains the in-process intercept (#99, ADR 0063), so
the `terminal` arm's `tny edit` is dispatched in-process rather than as a cold
nested process. Raw documents: `tests/bench/results/pilot-2026-09-02-*.json`.

Each run copies one fixture into a fresh scratch directory, feeds its
`task.md` to `tny ask -B --json --stdin`, blocks on
`tny session ID --wait --json`, and scores the scratch with the fixture's
`check.sh`. Every check is verified red before any work and green after a
reference solution (`--verify-fixtures`). Three families: make a failing C or
Python test pass (10), rename a function across three files (8), answer a code
question with a verifiable `file:line` written to `answer.txt` (8). No task
mentions a tool; the vocabulary each arm sees is entirely the profile's.

| Arm | Pass | Steps | Tool calls | Tok in | Tok out | Wall s | Repairs | tny edit | edit_file | shell write |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `all` | 26/26 (100%) | 4.3 | 5.7 | 3052 | 80 | 12.0 | 0 | 0 | 42 | 0 |
| `terminal+edit` | 26/26 (100%) | 4.1 | 4.0 | 1942 | 84 | 14.5 | 5 | 0 | 44 | 8 |
| `terminal` | 26/26 (100%) | 4.4 | 3.5 | 1864 | 75 | 10.7 | 10 | 18 | 0 | 12 |

Steps, tool calls, tokens and wall clock are per-run means; repairs and the
edit-method columns are totals over the arm's 26 runs. Tool vocabulary
actually used:

| Arm | Calls by tool |
| --- | --- |
| `all` | `read_file` 42, `edit_file` 30, `grep_files` 24, `list_files` 23, `terminal` 18, `write_file` 12 |
| `terminal+edit` | `terminal` 61, `edit_file` 44 |
| `terminal` | `terminal` 90 |

Per-family pass rate is 100% in every arm, so the frozen set does not
discriminate on success at this difficulty — it discriminates on cost.

### What the numbers say

- **Success is a ceiling, not a signal.** 78/78 runs passed. The tasks are
  small and deterministic by design; they measure how a profile gets there,
  not whether it can. Any claim that one profile is *more capable* is
  unsupported by this data.
- **The shell profiles cost about 39% fewer input tokens** (3052 → 1864) and
  **39% fewer tool calls** (5.7 → 3.5) for the same result. The `all` arm
  spends its extra calls on structured discovery — `read_file`, `grep_files`
  and `list_files` account for 89 of its 149 calls — where a shell arm chains
  the same work into one command with `&&`.
- **Step count is flat** (4.1–4.4 model calls) across all three arms, so the
  token saving is per-request context, not fewer turns.
- **Wall clock does not separate the arms** at N=1 (10.7 s – 14.5 s, with the
  cheapest and the most expensive arm both in the shell group). Provider
  latency variance dominates.
- **Repair loops rise in the shell arms** (0 → 5 → 10) and are almost entirely
  one specific mistake, not general fragility: 9 of the `terminal` arm's 10
  are a first `tny edit` invocation with the wrong calling convention —
  invented `--old`/`--new` flags, two FILE arguments, or a bare heredoc with
  no `*** SEARCH` fence. Every one recovered on the next call, because the
  verb's stderr prints the exact `printf … | tny edit FILE` example (ADR
  0064). The remaining repairs are `rg` exiting 1 on a genuine no-match.
- **Edit-method drift is real but minor.** With an exact-match editor
  advertised (`all`, `terminal+edit`) the model never wrote a file through the
  shell in `all` and did so 8 times in `terminal+edit`. In `terminal` it
  reached for `tny edit` 18 times against 12 shell writes, so the prompt's
  "never `sed -i`" instruction holds about 60% of the time.

### Cold versus warm `tny edit`

The `terminal` arm was also run on tny `0.3.3-24-g261552f`, the same task set
and harness immediately **before** the intercept merged, where each `tny edit`
is a cold nested process:

| `terminal` arm | Pass | Steps | Tool calls | Tok in | Tok out | Wall s | Repairs | tny edit | shell write |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| cold nested (`261552f`) | 26/26 | 4.6 | 3.6 | 1948 | 79 | 10.3 | 14 | 11 | 14 |
| in-process (`ac0bd11`) | 26/26 | 4.4 | 3.5 | 1864 | 75 | 10.7 | 10 | 18 | 12 |

The intercept moves the arm in the intended direction on every behavioural
column — more `tny edit`, fewer shell writes, fewer repairs — but the
differences are a handful of events at N=1 and the wall clock is a wash. This
is directionally consistent with ADR 0063, not evidence for it.

### Recommendation

**Do not change the default yet.** `all` stays the default in code; this ADR
records the measurement, not a switch (that decision is the user's).

On this evidence `terminal+edit` is the profile to move toward: it captures
essentially the whole token and tool-call saving of pure `terminal` (1942 vs
1864 input tokens, 4.0 vs 3.5 calls) while keeping the exact-match editor that
`terminal` has to rediscover through a CLI, which is where 9 of its 10 repair
loops came from. That matches the prior in the Context section — every
shipping agent keeps an editor beside the shell.

Two caveats bound this, plainly:

1. **N=1 on one model.** Every arm passed every task, so the comparison rests
   on cost means from single samples of a nondeterministic policy. The
   differences that matter (tokens, tool calls) are large and consistent in
   direction; the small ones (wall clock, individual repair counts) are
   inside the noise. N≥3 across at least a second model is needed before
   anything here justifies changing a default.
2. **The set is too easy.** A 100% ceiling in all three arms cannot detect the
   capability regression that SWE-agent's shell-only ablation found on harder
   work. Before the default moves, the set needs tasks that a profile can
   actually fail.

Two findings are actionable independently of the default:

- The shell-profile prompt block (ADR 0062 §5) names `tny edit FILE` but not
  its stdin fence. Nine first-attempt failures in 26 runs say the block should
  carry the one-line `printf '*** SEARCH\n…' | tny edit FILE` form verbatim.
- The `terminal` arm still writes files through the shell in 12 of 30 mutating
  commands despite the prompt forbidding `sed -i`. Prompt wording alone does
  not enforce the editor.

Reproduce with `docs/ci.md` → Benchmarks; the offline smoke is
`tests/integration/test_bench_tools.py`.
