# 0056 — Inject mentioned skills into the user turn, not the system prompt

Date: 2026-09-02
Status: accepted (relates to 0045 system-prompt flag, 0049 MCP catalog, 0053 forked turns)

## Context

Skills are discovered at startup as metadata only; the system prompt lists
`name: description` and the model loads a body with the `skill` tool
([mcp-and-skills](../features/mcp-and-skills.md)). When the user writes
`$deploy the branch` or `/deploy`, the intent is unambiguous, yet the model
still spends a full round trip (a tool call, tny executing it, a second
model call) before it has the instructions. Host backends (cursor, codex,
ACP clients) never see tny's `skill` tool at all, so there `$deploy` is just
text.

The question was where to put the body.

- **System prompt.** Rejected. Providers cache the system prompt per
  session; a per-turn skill body appended there invalidates the cache on
  every turn and would ride along on every later request.
- **Synthetic `assistant` tool call + `tool` result before the user turn.**
  Mirrors what would have happened anyway and keeps the user text clean.
  Rejected: it exists only in the native loop (hosts own their loops and
  expose no way to pre-seed a tool exchange); the Responses API rejects a
  `function_call` item with no prior model turn on strict routers; and it
  fabricates a model action in the transcript.
- **Reasoning trace.** Not injectable on any backend. Rejected.
- **The user turn itself.** One string, every backend accepts it, and it
  is exactly the seam the engine already uses for `--system-prompt` on
  hosts (0045) and for extension context. Chosen.

## Decision

1. **Matcher.** A prompt mentions a skill when it contains `/<name>` or
   `$<name>` as a whole token: at the start of the text or after
   whitespace, followed by the end, whitespace, or punctuation other than
   `/`, `-`, `_`. So `/foo` does not match `foobar`, `foo-bar`, `a/foo`, or
   the path `/foo/bar`, while `$foo.` and `/foo,` do. Names are exact,
   case-sensitive, and come from `skills_discover`. Several skills may match
   one message; each is injected once, in order of first appearance
   (`skills_mention_pos`, `skills_mentioned` in `src/core/skills.c`).
2. **Placement.** `tny_engine_start` (`src/core/runtime.c`) rewrites the
   effective prompt once, after `user_prompt_submit` extensions and before
   the host system-prompt prefix, so every entry point that sends a user
   prompt (`tny ask`, the TUI, `tny acp` server, the forked session runner,
   libtny embeds) and every backend gets the same text:

   ```text
   <skill name="deploy" path="/abs/path/skills/deploy/SKILL.md">
   ...SKILL.md verbatim...
   </skill>

   <the user's text, unchanged>
   ```

   The block precedes the text so the request the model acts on is the last
   thing it reads. The system prompt and its skill catalog do not change.
3. **Bound.** A body larger than `max_tool_result_bytes` is cut exactly like
   a tool result: half the budget, a `…[truncated: N of M bytes shown]`
   line, and on the native loop the `read_tool_result` handle from
   `tool_bound_result` plus a pointer to the `skill` tool; hosts get the
   file path instead, since they have no tny handles.
4. **Session record, not message shape.** The native backend stores the
   rewritten text as the user message — that is what the model saw and what
   must replay on later turns. The chat request serializer copies stored
   messages verbatim, so no display field may live on the message. Instead
   `session.json` gains a top-level `skill_injections` array,
   `{message, skills[], display}`, written only after the send was entered.
   The TUI `/transcript` and `tny session <id>` show `display` (the typed
   text) for that message index; compaction never deletes messages, so the
   index is stable. Host sessions hold no messages; the record still marks
   the skill as delivered.
5. **No re-injection.** A later mention of a skill whose record sits at or
   after the compaction boundary sends a one-line self-closing
   `<skill name=... status="already loaded earlier ..."/>` reminder instead
   of the body. After `/compact` moves the boundary past it, the body is
   sent again, because the summary dropped it.
6. **TUI routing.** A line starting with `/` is still a slash command when
   the first token is a builtin (`tui_command_is_builtin`); only an unknown
   `/name` that names a discovered skill becomes a prompt. `$name` anywhere
   is plain text and always reaches the backend. Steered text mid-turn is
   not scanned.
7. Library mode (`library_mode`) never injects, matching the absent catalog.

## Consequences

- One fewer round trip for the common case; nothing changes for a message
  that mentions no skill (a text with neither `/` nor `$` skips discovery
  entirely).
- `session.json` has a new optional top-level array; old sessions lack it
  and read as before.
- Hosts see a `<skill>` block they did not ask for. That is the same
  trade-off 0045 made for `--system-prompt`, and cheaper than teaching each
  host protocol about tny skills.
- Unit coverage: matcher edge cases and the fixture workspace
  `tests/fixtures/skills-ws` (`tests/test_skills.c`), plus an engine test
  through the fake backend that checks the exact prompt, the record, the
  reminder, and the non-mention cases (`tests/test_runtime.c`).
