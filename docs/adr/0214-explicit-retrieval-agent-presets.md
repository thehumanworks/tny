# ADR 0214: Explicit retrieval agent presets

Status: accepted

Goal: let a chosen specialist inspect large sources without copying them into the lead's context.

Success: local real-process fixtures retrieve evidence beyond 64 KiB, retain it across resume, and return bounded cited results on both HTTP wires. Semantic recall, real token savings, cost and latency require separately authorized live evaluations.

## Decision

Global `agent_presets` selects an existing provider, model and optional effort,
instruction task and output bound. It does not introduce credentials or another
agent loop. Workspace task Markdown remains instructions, not routing authority.
No preset is selected automatically and no default specialist model is supplied.

The existing `subagent` create action accepts `preset` and optional `sources`.
Sources are explicitly selected parent-owned result handles or qualified whole
transcript-message handles. Transcript materialization uses the session owner RPC;
ordinary result artifacts are reused. At most 32 sources and 64 MiB in aggregate
are imported. An internal launch manifest contains only identity, path, byte count
and integrity metadata. Source bytes never pass through stdin or initial prompts.
The child independently streams each source through the canonical atomic result
writer, checks the source digest and size before publication, and removes every
newly imported file if any import fails. A typed finish returns the owned handle
and exact byte count without parsing human-readable storage receipts. Original source identity remains separate from child-local handles.

This is bounded context selection, **not a security sandbox**. Existing tool and
filesystem permissions remain unchanged. Source text is untrusted evidence, not
instructions. Coverage is unknown unless trusted metadata establishes otherwise;
copying all retained bytes does not establish that an original web page was complete.

The profile, effective effort, resolved task instructions and source manifest are
frozen for the child. Resume uses that snapshot, not an edited named preset.
Changing the resolved endpoint, wire, sign-in mode or ACP command fails before
inference. Routing identity is hashed without storing credentials; token refresh
remains normal. Explicit raw provider/model/effort overrides conflict with presets.
Saved native sessions are required; ephemeral and unsupported process environments
fail clearly instead of silently losing durability. Generic `session recover` does
not copy retained result files, so retrieval sessions are explicitly refused rather
than producing an unusable recovered ID. Preserve the original session and source
files; normal resume remains supported. Native SDK callers may open a saved
specialist with matching explicit routing. Each send revalidates its retained
sources and task before inference. Closing it clears inherited specialist context
from the reusable runtime; an explicitly configured runtime task is preserved.

Specialist output is a JSON object with `summary` and `citations`. Each citation
names an original source and a byte `offset` and `length`. The harness validates
membership and bounds, **not semantic support**. Invalid envelopes are withheld.
An oversized valid envelope is stored intact with a recovery handle, never sliced
through its citation metadata. Originals remain retrievable from child-owned files.

## Costs and limits

Whole-source snapshots add bounded disk I/O and retention. Resume hashes retained
child copies again and fails before inference if any copy is missing or changed. The checksum is the
specified SHA256-chain-v1 over 64 KiB blocks, not an ordinary file SHA-256.
There are no ambient cross-session lookup grants or automatic specialist calls.
A specialist incurs another inference request and can be slower or more expensive;
user-selected subscriptions/models are subject to their existing authentication
and availability. Mock tests establish routing, retention, failure handling and
payload bounds only. A live comparison must report retrieval recall/section
selection, lead payload, extra latency, provider token accounting and actual cost
without treating deterministic fixture answers as model-quality evidence.
The fixture body-to-summary byte comparison is not a benchmark against the large-
content guard, which already returns a small artifact receipt. A live baseline
must compare specialist retrieval with the lead's direct bounded chunk reads.
