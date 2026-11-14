# 0217 — Standalone native tool CLI

Date: 2026-11-07
Status: accepted

## Context

Every capability should be usable outside the interactive harness. The agent
(`tny ask`), dictation, speech, images, web, edit, jobs, teams, mailboxes and
MCP already had standalone commands, and the SDK Toolkit covers image, speech
and transcription. The file, search, workspace, memory, skill, subagent and
image-export tools had no surface outside a turn; `tny memory` and `tny skill`
were documented but only existed as in-turn terminal intercepts.

## Decision

Add one generic command instead of a bespoke verb per tool:
`tny tool list | describe NAME | call NAME [ARGS_JSON|-]`. It mirrors the
model's own `tools.search/describe/call` and runs `tools_execute` against the
nested catalog, so permissions, workspace roots, `--ssh` routing and result
bounds are the agent's. New tools become reachable without CLI work.
`tny memory` and `tny skill` become thin top-level shortcuts over the same
calls.

Tools tied to a live turn (`read_image`, `read_tool_result`,
`search_transcript`, `ask_user_question`) are hidden and refused rather than
silently discarding their effect. No provider is resolved unless `--provider`
is given. Inside a turn, `tny tool` through the terminal tool is refused so a
call never hides its permission identity behind `terminal`.

## Consequences

Scripts get every native tool with one stable JSON envelope and exit-code
contract. The SDK equivalent is ADR 0223. Bespoke verbs stay where
they add ergonomics (`tny web`, `tny edit`, `tny image`).
