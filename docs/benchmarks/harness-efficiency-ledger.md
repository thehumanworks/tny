# Experiment ledger (tny harness optimisation, 2026-09-24)

Model gpt-6-sol, effort medium, codex (ChatGPT) backend via recording proxy,
unless stated. ITE: uncached 1, cached 0.1, cache write 1.25, output 5.

| # | Experiment | Arms | N | Result | Decision |
| --- | --- | --- | --- | --- | --- |
| P1 | Model pilot | tny, codex × gpt-6-luna / gpt-6-sol × 4 tasks | 8 valid | luna failed cli-feature on both harnesses; sol passed all; ~$0.05–0.09/task on sol | Primary model gpt-6-sol medium |
| F1 | `TNY_EXP_BATCH_HINT=1` (same-step tool-call line) | off vs on, 3 tasks × 2 reps | 6 vs 6 | pass 6/6 both; ITE/task 32.7K vs 32.7K; requests 10.0 vs 9.5 | Null at this N; not pursued |
| F2 | `TNY_EXP_VERBOSITY=low` | off vs low, 3 tasks × 2 reps | 6 vs 6 | pass 6/6 both; ITE/task 32.7K vs 36.7K; output 1575 vs 1647 | Null/negative; not pursued |
| F3 | `TNY_EXP_REASONING_CTX=all_turns` | — | — | Only affects multi-user-turn sessions; benchmark is single-turn | Not measurable here; documented only |

## Observations

- tny cost decomposition on small tasks (flags-off, 6 runs): uncached 48%,
  cached 28%, output 24%, hit rate 85.5%. First request caches ~3.7K of
  ~6.1K static tokens (per-workspace bytes early in instructions). Mid-turn
  cache misses: 19/177 follow-up requests (11%).
- Codex for gpt-6 sends tools as an `additional_tools` developer input item
  (a single JS `exec` custom tool in a V8 isolate plus wait/agent tools),
  `parallel_tool_calls:false`, `reasoning.context:"all_turns"`,
  `text.verbosity:"low"`, `prompt_cache_key` = thread id. Code mode lets one
  request fan out many operations; tny averages ~1.1 tool calls per request.
- Static prefix (smoke, tokens): unreal-agent 534, pi 1411, fx 5037,
  tny 6119, omp 6767, codex 8634, hermes 13014.
- Real sessions: 171 user turns, mean 14.8 requests/turn (p50 7, p75 19,
  p90 38, max 129). 44% of all requests are in turns with ≥40 requests.
  Mean context 110.8K is session-level accumulation across turns plus
  in-turn growth.
- ctxedit mock (60-step turn): input bytes 30.25 MB → 11.30 MB (−63%), 5
  clearing batches, 6 distinct prefixes, payback 20–28 requests per batch.
  Trigger 48K tokens: never fires on the short task suite.
- Review finding (ctxedit): flags parsed into tny_ctx do not reach the
  detached session runner unless carried in the checkpoint start packet;
  ctxedit's mock gains were from the in-process path only. getenv-based
  flags (F1/F2) do reach the runner: request bodies confirm 6/6 firing.
- Review finding (ctxedit): test_ephemeral fails on main's binary too
  (pre-existing); ctxedit flag-off request bytes identical to main (61/61).
- Review finding (toolerr): first cut regressed ignore rules for explicit
  root paths (70x slower repo-root grep, credential files surfaced) and
  broke literal searches via regex guessing; the replay metric (95→13
  no-match) hid it because fixtures stripped special characters. Sent back.

## Baseline cross-harness matrix (label `baseline`, tny-baseline @ f90fc2d)

gpt-6-sol medium, 12 tasks × 3 reps, 252 runs, all harnesses 35–36/36 pass.
USD/task: unreal-agent 0.0346, pi 0.0411, tny 0.0683, omp 0.0697,
codex 0.0760, fx 0.0991, hermes 0.3195. tny requests/task 9.6 (unreal 5.7,
pi 7.7, codex 8.1); static prefix tny 6,110 (unreal 534, pi 1,404).
tny ITE split: uncached 14.8K, cached 10.4K, output 8.9K per task.
Final messages are ~300 chars for all; output cost is tool args + reasoning.
Unreal prompt states "each turn re-sends the whole conversation, so prefer
to go wider"; pi batches multiple edits per edit call (1.8 calls/request).

| # | Experiment | Arms | N | Result | Decision |
| --- | --- | --- | --- | --- | --- |
| A1 | `TNY_TOOLS=terminal+edit` (existing profile) | vs baseline, 12 tasks × 3 | 36 vs 36 | 36/36 both; ITE ratio 0.967 [0.83, 1.14]; context −28%, output +36%, requests +8% | Null; not a default change |
| A2 | `TNY_TOOLS=terminal` | vs baseline | 36 vs 36 | 36/36 both; ITE ratio **1.149 [1.01, 1.29]** (worse); output +51% | Rejected |

Insight: removing read/edit/write tools moves work into shell heredocs;
output tokens cost 5x, erasing the prefix saving. Offload only rarely used
tools (prefix worker), keep file tools.
- Review (ctxedit re-review): merge. Default-isolation bench fires 5
  batches (30.25→11.35 MB Responses; 30.45→11.55 MB Chat); flag-off 61/61
  request bodies byte-identical to main on both wires.
- Review (compact): not mergeable yet: F1 stale pending summary after
  cancel, F2 screenshots counted as user prompts (drops real prompt,
  reproduced), F3 no Chat usage so never fires + usage reset to 0, F4 archive
  failure breaks turns. Flag-off identical to main (20/20). GCC analyzer
  failure on main's jobs.cpp/ownership.hpp/resources.hpp is pre-existing
  (9 identical diagnostics).
| A3 | `TNY_EXP_BATCH_HINT=2` (Unreal-style step-cost wording) | vs baseline, 12 × 3 | 36 vs 36 | 36/36 both; fired 36/36; requests ratio **0.919 [0.84, 0.985]**; ITE ratio 0.994 [0.86, 1.13] | Candidate for stack; confirm in interleaved run |
- Mid-turn cache misses are provider-side: at every tny miss the
  instructions/tools are byte-identical and the previous input is an exact
  prefix. Miss rates (gpt-6-sol baseline, follow-up requests): tny 9.7%,
  codex 7.0%, omp 10.4%, pi 29%, unreal 54.8%. Not a tny lever.
- Long tasks (3 × 3, gpt-6-sol): pass tny 6/9, codex 6/9, pi 6/9, unreal
  8/9; USD/task unreal 0.056, pi 0.086, codex 0.154, tny 0.175. tny requests
  22.8 (unreal 6.4); triage-suite tny 44 requests/69.7 calls vs unreal 5.7/6.7.
  Peak context ≤20K: ctxedit (48K trigger) never fired; long-ctxedit run is
  an A/A: ITE ratio 1.019 [0.85, 1.39] = long-task noise floor.
- ctxedit stress arm (TNY_EXP_CTX_EDIT_TRIGGER=12000, long tasks 3×3):
  0 clearings. Peak context ≤25K, 9–37 requests: the eligibility/payback
  gate correctly refuses clearing that would not pay back. No live effect
  measurable on single-prompt tasks. → building multi-turn session tasks.
- Spill arm (TNY_EXP_SPILL=1, 12×3): 36/36; ITE ratio 0.894 [0.854, 0.931];
  fired: spill in 5 runs, read continuation in 6. CONFOUND: mid-turn miss
  rate 6.9% vs baseline 9.7% (provider-side), first-request warmth 8.4K vs
  7.3K; ~1.7K of the ~4K ITE/task saving is miss variance. Needs
  interleaved confirmation (baseline and treatment concurrently).
- tasks-long audit: triage-suite failures (all harnesses) come from two
  hidden checks the repo never states; unreal's 2/3 = vocabulary luck.
  With fixed oracle all 12 saved runs pass. api-migration/incident-report
  do not discriminate on success. Fix + rescore assigned to tasks worker.
- Review (results): do not merge as-is: tail of long final line lost,
  SSH/ask-mode spill paths unreadable (no handle), flag-off byte drift in
  512 KiB clamp (size metric hid it), read_file single huge line returns no
  content. Sent back.
- Long tasks rescored with fair triage oracle (repo/prompt unchanged):
  all harnesses 9/9. USD/task unreal 0.056, pi 0.086, codex 0.154,
  tny 0.175. tny is the most expensive on long tasks.
- Driver: unprompted delegation. 3/9 tny long runs (all triage-suite)
  spawned subagents: 57% of long-task ITE; mean ITE 150.6K (44 requests)
  vs 56.0K (12.2 requests) without. tny's prompt line "When delegation is
  available and worthwhile, give independent tasks clear context and
  ownership" invites it; Codex's prompt says do not spawn sub-agents unless
  asked. → A4: TNY_EXP_DELEGATION=off (drop that line), interleaved vs
  fresh baseline on long tasks.
- Review (compact re-review): F1–F11 fixed; new blocker R1 (base64
  screenshot inflates the bytes/4 estimate and stops compaction). Both
  flags on: sane ordering. Flag-off 20/20 identical. ACP make-test failures
  are environmental: mise python3 shim + untrusted worktree .mise.toml;
  `mise trust` fixes them.
| A4 | `TNY_EXP_DELEGATION=off` (drop delegation push line) | interleaved vs fresh baseline, long 3×3 (rescored, fair oracle) | 9 vs 9 | 9/9 both; subagent runs 5/9 → 0/9; requests 24.6 → 11.4; **ITE ratio 0.550 [0.41, 0.64]**; USD/completed 0.187 → 0.101 | **Ship (default on)**: biggest single win |
- Session pilot (12-turn resume, gpt-6-sol, 1 rep): tny peak context
  54.9K; turn-first cached fraction 0.91–0.99 for turns 1–8, then
  **0.25–0.43 from turn 9 on** (turn-count compaction rewrites the prefix
  every turn), codex stays 0.99. USD/session tny 1.19, codex 1.04.
  commerce-release failed on both (audit requested).
- tasks-session audit: commerce-release failures (tny and codex) are two
  oracle defects (unported retry fix; exact-phrase passing count that
  punishes adding tests). Agents' work is correct. Oracle-only fix +
  rescore assigned. policy-migration: minor (known false pass, strict notes).
- Session pilot rescored with fair oracle: tny 2/2, codex 2/2.
| S1 | `TNY_EXP_COMPACT=1` (128K trigger) on 12-turn sessions | interleaved vs s-base, 2 tasks × 2 | 4 vs 4 | 4/4 both (rescored); turn-first cached 0.25–0.39 → 0.99 for turns 9–12; uncached/session ~200K → ~97K (−52%); mean context 34.7K → 44.0K (history kept instead of 4-turn truncation); ITE ratio 0.992 [0.95, 1.04]; compaction never fired (peak 60–77K) | Cache-bust fix proven; cost-neutral at this scale; testing 48K trigger |
| S2 | `TNY_EXP_DELEGATION=off` on sessions | vs s-base | 4 vs 4 | 4/4; ITE ratio 1.027 [0.91, 1.16] | Null on sessions (no subagent use there) |
- Main baseline gates on this host: `make quality` stops at lint-sh
  (zsh not installed; env). `make test`: 7 integration modules fail
  (acp_client, acp_managed, collective_swarm, image_preview_workflow, jobs,
  swarm_delivery, swarm_parent) because child fixtures run through mise's
  python3 shim; with PATH=/usr/bin:$PATH acp_managed passes 19/19. All gate
  comparisons use PATH=/usr/bin:$PATH.
| A5 | `TNY_EXP_PREFIX=1` (tool offload + prompt audit + cache layout) | interleaved vs fresh baseline (base2), short 12×3 | 36 vs 36 | 36/36 both; static 6,109 → 1,379 tokens; context −32% (0.678 [0.65, 0.71]); **ITE ratio 0.856 [0.765, 0.957]**; USD/completed 0.0603 → 0.0540; requests 0.976 (null); tool_search never called | **Ship (default on)** |

Stack plan for confirmation: prefix + delegation-off + toolerr + compact
(cache-bust fix) + spill (pending review + confirmation); ctxedit default
off (no live effect measurable); batch hint dropped (ITE null).
- Main gate baseline (PATH=/usr/bin:$PATH): all 7 previously failing
  integration modules pass → main `make test` clean. `make quality`:
  lint-sh needs zsh (absent); analyze-cpp-gcc diagnostics in jobs.cpp /
  ownership.hpp / resources.hpp pre-existing (reviewer confirmed identical).
- Review (results re-review): mergeable behind flag; flag-off byte parity
  in 7 scenarios; 100 MB output peak RSS 133 → 21 MiB. Remaining small:
  non-final long line tail, negative offset semantics, >8 MiB end.
| S3 | `TNY_EXP_COMPACT=1 TNY_EXP_COMPACT_TOKENS=48000` sessions | vs pooled baseline (7 valid runs) | 4 | 4/4 pass; USD commerce 1.52 → 1.26, policy 1.08 → 0.92 (≈ −15%); mean context 35–37K → 31K | Candidate default for stack confirmation |
- Fairness bug fixed: adapter set TNY_PROVIDER_RETRIES=0 for tny only; a
  504 aborted one baseline session (excluded as infra). Final confirmation
  runs from a revision with tny default retries.
- Review (prefix): 4 must-fix (tool_search substring over-loading; name
  shadowing for embedders; "for this turn" wording; direct-call hint),
  append-order, cache-key-scope question. Flag-off identical (12 requests,
  default isolation); firing through runner, subagents, resume verified.
