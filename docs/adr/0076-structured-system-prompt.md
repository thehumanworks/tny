# 0076 — Structured native system prompt

Date: 2026-09-06
Status: accepted

## Context

The native prompt supplied a role, workspace, tools, and a generic Markdown
instruction, but left completion, verification, and reporting expectations
implicit. The user needs short status reports that remain clear when switching
between projects. The [GPT-6 Astra guide](https://developers.openai.com/api/docs/guides/latest-model.md)
recommends explicit autonomy, instruction precedence, and proportionate testing.

## Decision

- Use short bullet directives grouped under execution, instructions,
  verification, and communication. Keep blocker handling in execution and
  testing requirements in verification, without repeating them in communication.
- Complete authorized work, make reasonable assumptions, preserve existing work,
  and resolve blockers where possible. Ask for necessary user input at the end,
  with a recommendation and its tradeoff.
- For code changes, create or update relevant tests/QA checks and run them.
  Complete required project checks; expand or repeat verification when evidence
  warrants it.
- Use simple technical English and concise, decision-relevant reporting. Prefer
  a compact work/checks/blockers table, identify partial or unverified work, and
  honor explicit output formats. Limit progress updates to meaningful changes.
- Keep environment facts separate from behavioral directives. Include the
  effective tool profile and permission mode alongside existing workspace/SSH
  information. Preserve project, skill/MCP, task, and custom prompt composition.
- Name `tny skill show NAME` in shell profiles, where the structured `skill`
  tool is hidden. Prefer focused file reads without forbidding whole-file reads.

## Consequences and verification

The same core reaches Responses `instructions` and the Chat Completions system
message, including the builtin Codex native profile. Cursor and ACP host prompts
remain host-owned. wasm and libtny use the shared core without gaining new tools;
existing profile and capability gates remain authoritative.

The communication block is shorter than the reviewed draft; the overall core is
longer than the old minimal preamble because it now states the user's working
contract. No token, latency, or task-success improvement is claimed without a
controlled model evaluation.

The OpenAI integration fixture checks both wires and all three tool profiles,
including the communication/verification directives, effective profile, lazy
skill catalog, and absence of the hidden skill route in shell profiles. The
mock applies prompt assertions to both wires. Existing SSH, task-preset, custom
system-prompt, Codex, and tool enforcement fixtures cover composition regressions.
