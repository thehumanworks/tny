# ADR 0082: Independent prompt optimisation before submission

Status: accepted. Date: 2026-09-09.

## Context

Typed and dictated drafts benefit from relevant project context before being
sent to the coding agent. The rewrite model must be configurable separately
from the conversation, and inspecting a project must not execute the draft.

## Decision

- Add `/optimise PROMPT` and Ctrl-O, plus `tny optimise` for pipelines. TUI
  results replace the editable draft; only the user's later Enter submits.
  Preserve original text on cancellation or failure. Keep `/transcript` for
  the previous Ctrl-O action.
- Default to OpenRouter `inception/mercury-2.5`. Resolve per-request options,
  service-specific environment variables, and the global `optimise` settings
  object independently of the conversation's provider, model, and task.
- Reuse the native lifecycle engine, HTTP transports, tool loop, and event
  vocabulary. The service owns a separate explicit context and ephemeral
  session. As with other ephemeral turns (ADR 0053), it runs in-process;
  there is no durable draft turn to recover or session runner to preserve.
- Enforce an internal allowlist of file-reading/search tools in both schema
  advertisement and execution. Do not inherit MCP, extensions, custom tools,
  task presets, or automatic skill-mention injection. Restrict overrides to
  native providers so this contract remains enforceable.
- Give the model a dedicated rewrite instruction with selective project
  exploration, intent preservation, and prompt-only output. Project context
  is reference material; it cannot grant authority to execute the draft.
- Bound the draft/result to 64 KiB, the loop to 12 steps and 120 seconds
  between engine steps, and tool results to 16 KiB. Respect smaller parent
  limits. Use existing cancellation, UTF-8/control validation, and `tny_poll`.
- wasm uses the same service and its visible filesystem; browser host files
  remain unavailable unless provided to that filesystem. No new platform seam.
- The Node wasm bootstrap connects stdin readiness to the existing poll seam
  lazily, on the first stdin poll. This fixes the pre-existing piped TUI hang
  while keeping synchronous CLI stdin reads unchanged. JavaScript handlers
  only wake the event loop; they never re-enter C.

## Verification

`test_optimise.py` drives the native loop through local HTTP fixtures and
native PTYs, checking nested discovery/readback, tool restrictions, model/key
separation, override precedence, output bounds, cancellation, draft review,
and explicit subsequent submission. The CLI fixture suite also runs in wasm
CI. Existing dictation, tool-schema, and TUI suites protect adjacent behavior.
