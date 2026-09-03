# 0045 — `--system-prompt`: native field where one exists, first-message prefix where none does

Date: 2026-08-29
Status: accepted. Codex references are historical: [ADR 0065](0065-codex-chatgpt-responses-backend.md) turned `codex` into a builtin profile of the native loop (no host process). `--provider codex` now carries `--system-prompt` on `instructions` like every openai profile.

## Context

Users embedding tny in scripts (and steering interactive sessions) want to
supply their own system prompt. The four providers disagree on whether a
system prompt is even expressible:

- **openai-compatible** has a first-class field: the `system` role message on
  the chat wire, `instructions` on the responses wire.
- **cursor sdk.v1** `CreateAgent` options carry model/apiKey/local/cloud only
  ([cursor-bridge.md](../backends/cursor-bridge.md)); no instructions field.
- **codex app-server** `thread/start` documents model/cwd/approvalPolicy/
  sandbox/personality/serviceName on the pinned surface; no instructions
  field (historical; see [codex.md](../backends/codex.md), and the
  repo rule is to never guess fields the schema does not pin).
- **ACP** `session/new` takes `cwd` + `mcpServers`; no system prompt at
  protocolVersion 1.

## Decision

One leading global flag, `--system-prompt TEXT`, valid for headless (`ask`)
and interactive (TUI) runs alike, stored on `tny_ctx`.

- The **openai backend** appends the text to the system preamble it already
  builds (`build_system_prompt`), so it rides the provider's native field on
  both wires after tny's runtime/safety instructions, project context, task
  preset, AGENTS.md chain and skill catalog (ADR 0048).
- Every **host backend** (cursor, codex, acp) gets the fallback the flag
  promises: the engine prepends `TEXT + "\n\n"` to the **first user message
  of a fresh session**. When a task preset is selected, its section precedes
  this explicit addition (ADR 0048). The injection lives in `tny_engine_start`
  (one shared spot for CLI, TUI, ACP server and libtny), not in each backend.
- The prefix is delivered at most once per conversation: never on later
  turns, never when the session already has recorded turns, and never when a
  host resume pointer exists (resumed or adopted threads already had their
  first message). Extension `user_prompt_submit` hooks run on the user's
  text first; the prefix is added after folding so prompt audits record what
  the user actually submitted. The session title also stays the user's text.

## Consequences

- `tny --system-prompt "…" ask "hi"` works identically across providers from
  the user's point of view; on host providers the text is visible as part of
  the first user message in the host's own transcript, which is the honest
  representation of what the protocol allows.
- If a future pinned codex/cursor/ACP schema grows an instructions field, the
  backend can claim the mapping by reading `ctx->system_prompt` itself; the
  engine fallback is keyed off the backend id and would be lifted then.
- Steering text and extension continuation turns never carry the prefix.
