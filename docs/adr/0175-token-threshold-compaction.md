# ADR 0175: Token-threshold, cache-friendly compaction

Status: proposed

## Decision

`TNY_EXP_COMPACT=1` enables native HTTP compaction based on the last reported
input-token count, at `TNY_EXP_COMPACT_TOKENS` (default 128000) or 80% of a
known context window, whichever is lower. A caller can supply a known window
with `TNY_EXP_COMPACT_CONTEXT_WINDOW`. The trigger is checked before each
provider request, including requests inside one agentic turn. ACP is unchanged.

The harness asks the selected model for a compact handoff using the existing
conversation prefix plus a final user message. The request keeps the same
system instructions, model, and conversation order, so the prefix remains
cacheable. No tool call is permitted in this one request. The provider view
afterward contains recent original user messages (about 16K tokens), the
summary as the last item before fresh work, and the two latest complete tool
batches when compaction occurred inside a turn. The stored transcript is
never truncated. Saved sessions write a pre-compaction transcript snapshot
and include its path in the summary. The summary is byte-stable until the
next compaction. Session records and extension `pre_compact` / `post_compact`
events include token estimates. Failed summary requests use the existing
mechanical summary content and resume the ordinary request.
Cancellation abandons a pending summary and its cut. Archive-write failure
degrades to a summary without a file path. A missing usage report retains the
last input-token count; Chat streaming requests usage explicitly. An overflow
error uses one mechanical fallback and retries because another request with
the same oversized prefix could not summarize it. A new summary waits for at
least four transcript messages and about a quarter-window of growth after the
previous one. Provider-visible tool screenshots and runtime context carry
flag-gated source markers in the session, stripped before HTTP serialization,
so the verbatim budget keeps only actual user prompts.

With context editing enabled, compaction runs first. No result-clearing pass
is made immediately before or after a summary request; summary usage does
not set the clearing trigger. Normal later steps can clear older tool results.
Compaction emits visible status at start and completion.

The flag-off arm keeps the existing turn-count policy and wire bytes. Both
Responses and Chat Completions use the shared session view. Native subagents
inherit the flag through their environment. Normal native turns run in a fresh
session runner, so the flag, threshold, and known window travel in its private
start/restart context packet only when enabled. Public recovery records and
flag-off packets gain no experiment fields. The same C path runs on wasm;
ephemeral sessions keep the transcript only in memory and omit the file path.

## Local mock measurements

The stdlib-only fixture `tests/integration/test_exp_compact.py` with
`--measure-isolated` runs normal detached session runners and returns input
bytes for every request and summary counts. It models usage as
request bytes divided by four and tests a 20-user-turn conversation with a
small tool call in each turn, then one 120-step turn with tool outputs of
4–20 KiB. The existing terminal-result offload leaves an 8 KiB inline preview
for larger outputs. Values below are local wire-size measurements, not provider
billing or live model quality.

| Scenario | Flag | Requests | Total input bytes | Mean bytes/request | Distinct first-request prefixes | Compactions |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 20 turns | off | 40 | 290,412 | 7,260 | 13 | 12 mechanical |
| 20 turns | on | 40 | 345,760 | 8,644 | 1 | 0 |
| 120 tool steps | off | 121 | 56,795,152 | 469,381 | 1 | 0 |
| 120 tool steps | on | 122 | 30,243,881 | 247,901 | 1 | 1 model |

The 20-turn on arm sends more total bytes because it retains the complete
history below the token threshold; it avoids twelve early-summary prefix
rewrites. The long-turn arm reduces cumulative bytes by about 47% with one
compaction request. These figures do not establish task-success parity or
actual cache billing. The raw per-request byte arrays are emitted by the
fixture and kept outside this ADR because they are generated measurements.
The same mock with `--compare-main` compared all 40 flag-off HTTP request
bodies to a Release binary built from `main` at `41a3b828`, using one server
and workspace under default isolation. They matched byte for byte (both
concatenated SHA-256 values:
`55458d972aabf3a66026e8150d8d2d4ea1116046023173e13d9cb57623b45259`).
The Chat wire also matched all 40 requests (both SHA-256 values:
`9f1a0986926f0488642868093b8086b8cfeaa9337be369b7a88b1344976949f7`).
With both experiment flags on in a separate eight-step detached-runner mock,
the first summary request was request 3, one compaction and five context edits
were recorded, and the summary and following request had no newly cleared
tool-result stubs. Request bodies were 4,678, 13,127, 22,172, 21,992,
30,585, 30,985, 27,145, 31,640, 32,183 and 32,583 bytes.

## Rollback

Unset `TNY_EXP_COMPACT` to restore the original policy and request layout.
The extra session metadata and archived transcripts are ignored by the
flag-off provider view. No saved messages need migration.
