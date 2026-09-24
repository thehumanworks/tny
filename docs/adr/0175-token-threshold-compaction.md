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

The flag-off arm keeps the existing turn-count policy and wire bytes. Both
Responses and Chat Completions use the shared session view. Native subagents
inherit the flag through their environment. The same C path runs on wasm;
ephemeral sessions keep the transcript only in memory and omit the file path.

## Local mock measurements

The stdlib-only fixture in `tests/integration/test_exp_compact.py --measure`
returns input bytes for every request and summary counts. It models usage as
request bytes divided by four and tests a 20-user-turn conversation with a
small tool call in each turn, then one 120-step turn with tool outputs of
4–20 KiB. The existing terminal-result offload leaves an 8 KiB inline preview
for larger outputs. Values below are local wire-size measurements, not provider
billing or live model quality.

| Scenario | Flag | Requests | Total input bytes | Mean bytes/request | Distinct first-request prefixes | Compactions |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 20 turns | off | 40 | 290,412 | 7,260 | 13 | 12 mechanical |
| 20 turns | on | 40 | 345,760 | 8,644 | 1 | 0 |
| 120 tool steps | off | 121 | 56,790,796 | 469,345 | 1 | 0 |
| 120 tool steps | on | 122 | 30,241,572 | 247,882 | 1 | 1 model |

The 20-turn on arm sends more total bytes because it retains the complete
history below the token threshold; it avoids twelve early-summary prefix
rewrites. The long-turn arm reduces cumulative bytes by about 47% with one
compaction request. These figures do not establish task-success parity or
actual cache billing. The raw per-request byte arrays are emitted by the
fixture and kept outside this ADR because they are generated measurements.

## Rollback

Unset `TNY_EXP_COMPACT` to restore the original policy and request layout.
The extra session metadata and archived transcripts are ignored by the
flag-off provider view. No saved messages need migration.
