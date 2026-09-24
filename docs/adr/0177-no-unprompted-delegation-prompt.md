# 0177 — Remove the unprompted-delegation line from the system prompt

Date: 2026-09-24
Status: accepted

## Decision

The built-in system prompt no longer contains the line "When delegation is
available and worthwhile, give independent tasks clear context and
ownership, then collect their results." The `subagent` tool, its schema,
and explicit team and swarm modes are unchanged. The model still delegates
when the user, project instructions, a task preset, or a swarm/team mode
asks for it.

## Why

The harness-efficiency benchmark
([docs/benchmarks/harness-efficiency.md](../benchmarks/harness-efficiency.md))
found that on multi-module tasks the model read the line as an invitation to
fan out work that one agent finishes cheaper. Each child pays its own static
prefix and rebuilds context the parent already had. Codex's gpt-6 prompt
says the opposite ("Do not spawn sub-agents unless the user or applicable
AGENTS.md asks"), and the reference guidance in
`docs/web-resources/x_cursor_harness_optimisation_prompt.md` §6 recommends
removing prompting that pushes a model to delegate.

## Measurements

gpt-6-sol, effort medium, ChatGPT backend through the recording proxy,
default isolation. Paired by task, bootstrap over tasks, arms run
concurrently (interleaved) to control provider drift.

| Long tasks (3 tasks × 3 reps) | Before | After |
| --- | ---: | ---: |
| Pass (fair oracle) | 9/9 | 9/9 |
| Runs that spawned sub-agents | 5/9 | 0/9 |
| Requests per task | 24.6 | 11.4 |
| USD per completed task | 0.187 | 0.101 |
| ITE ratio after/before (95% CI) | — | **0.550 [0.410, 0.644]** |

Short tasks (12 × 3) and 12-turn sessions (2 × 2) never delegated in the
baseline; the change is cost-neutral there (session ITE ratio 1.027
[0.908, 1.162], 4/4 pass in both arms).

## Null results recorded with this decision

Measured with the same harness, not adopted:

| Change | Result |
| --- | --- |
| `text.verbosity: "low"` (Codex default) | ITE 32.7K → 36.7K per task, 6/6 both (N=6); not adopted |
| One line: tool calls in one response run in the same step | requests 10.0 → 9.5, ITE unchanged (N=6) |
| Unreal-style wording: each step re-sends the conversation, go wider | requests ratio 0.919 [0.84, 0.985], ITE ratio 0.994 [0.86, 1.13] (N=36) |
| `TNY_TOOLS=terminal+edit` profile as default | context −28% but output +36%; ITE ratio 0.967 [0.83, 1.14] |
| `TNY_TOOLS=terminal` profile as default | output +51%; ITE ratio 1.149 [1.01, 1.29] (worse) |
| `reasoning.context: "all_turns"` | not measurable with single-prompt tasks |

## Rollback

Restore the removed line in `build_system_prompt`
(`src/backends/openai/openai.c`). No settings or session formats changed.
