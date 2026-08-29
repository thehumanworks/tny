# 24 — Cursor SDK mapping: type table and shared field aliases

Medium-low. From the complexity review. Independent of 20–23. Do **not**
invent a plugin system or collapse unwrap/dedup.

`handle_sdk` (CC ~29) and `emit_tool` in `src/backends/cursor/map.c`
spend most branches on alternate JSON spellings (`tool_call` /
`toolCall`, `callId` / `call_id` / `toolCallId`, `args` / `rawArgs` /
`arguments` / `input`, …). Protocol mess is real (live sdk 1.0.28).
Accidental complexity is repeating `str2` / `jget` fallbacks and cloning
text/thinking/status collect+emit.

## Work

- Add a tiny “first present field” helper and use it uniformly for the
  documented alias sets.
- Add a `type → kind` table for event types (`assistant`, `thinking`,
  `tool_call`, …) so `handle_sdk` is not a type soup. Keep unwrap/dedup
  explicit.
- Leave `tests/test_cursor.c` union/envelope/legacy shapes as the
  contract; add a case only if a currently accepted alias is no longer
  reached through the helper.

## Acceptance

- Every fixture in `tests/test_cursor.c` still maps to the same event
  kinds and tool ids.
- A new sdk spelling for `call_id` is one alias in the helper, not a
  new branch in `emit_tool` and `handle_sdk`.
- Unwrap/dedup comments and behavior stay; this is not a rewrite of the
  Cursor backend.
