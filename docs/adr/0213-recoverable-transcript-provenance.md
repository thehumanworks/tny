# ADR 0213: Recoverable transcript provenance

Status: accepted

Date: 2026-10-21

## Goal

Let an agent recover original evidence behind compacted context without loading
an entire conversation into its prompt. This is the retrieval/provenance
foundation, **not automatic semantic compaction**.

## Decision

Extend the existing session owner, result reader and execution RPC. Do not add
a memory database, global session search or another transcript archive.

- `search_transcript` browses original messages when `query` is empty; otherwise
  it performs literal, case-sensitive substring search over message JSON.
  It scans at most 256 originals and returns at most 32 descriptions. Results
  include session ID, original index, role, a UTF-8-safe 160-byte excerpt,
  serialized byte size and a recovery handle. Follow `next_index` until
  `complete`; an empty page is not proof of absence while incomplete.
- `transcript:message:SESSION:INDEX:DIGEST` identifies the complete original
  message JSON, including tool-call IDs, arguments and result metadata.
  `read_tool_result` retrieves bounded byte ranges. The session qualifier
  rejects accidental use in another session; the digest rejects edited
  sources. It is an integrity check, not a cryptographic authorization token.
  Existing tool-body `transcript:INDEX:DIGEST` handles remain unchanged.
- IDs remain stable across append, compaction, save and resume. Original
  message arrays are not rewritten. No summary or retrieval operation changes
  permission grants, tool pairs or the provider's recent verbatim context.
- Execution snapshots do not gain historical messages. A bounded metadata RPC
  consults the owning session; existing bounded result RPC retrieves bytes.
  Owner-dependent reads remain serial barriers in `tools.parallel`.
- An internal explicit materialization helper validates a selected message
  once and copies its JSON into the existing result store, returning only a
  session-qualified artifact handle and byte count. Subsequent ranges seek
  that artifact. This supports selected-source import without quadratic JSON
  reserialization, not speculative caching of every browsed message. Retention
  is capped at 64 MiB on disk or the existing 1 MiB ephemeral aggregate budget.
  It grants no cross-session access. Ephemeral execution snapshots publish local
  spills before materialization and reserve owner-only artifact bytes from the
  returned available-memory count, without copying source bodies. Bounded
  streams recheck that shared budget when committing; legacy unbounded result
  storage is not redefined.
- Explicit `/compact` records original session and `[first,end)` source
  indices. Each appended mechanical excerpt block points directly to its
  original range, not another summary. Its recovery guidance distinguishes
  incomplete excerpts from instructions or verified outcomes. UTF-8 clipping
  is boundary-safe and allocation failure leaves the prior compact record.

## Limits and compatibility

The metadata bounds are output/count bounds, not a hard per-message CPU or
allocation budget. Search serializes each scanned original separately; direct
whole-message range reads serialize the selected message before slicing.
Explicit materialization avoids repeating that work for bulk selected-source
imports. It does not reduce the existing owner's whole-transcript memory.

Search is exact JSON substring search, not semantic or Unicode case-folded
search. Excerpts describe the beginning of a message, not necessarily its
matching region. A model may still fail to retrieve the right evidence.

`search_transcript` is read-only under the existing permission engine, and
available in the full tool profile. Restricted terminal-only profiles keep
restricting it and `read_tool_result`; compaction does not widen authority.
Ephemeral sessions use only their in-memory originals/artifacts. Native HTTP
Chat and Responses share the implementation. ACP can inspect tny-owned
messages but this does not expose an external provider's private transcript.
Wasm retains the existing clean unsupported execution-tool behavior; no new
OS seam or public ABI is introduced.

## Verification and semantic-compaction gate

Focused tests cover exact whole-message recovery, source identity after
save/resume/append, wrong-session and edited-source rejection, bounded search
cursors, artifact recovery, Unicode persistence, and private RPC admission.
The production execution fixture exercises both HTTP wires, two resumes and
more than 10 MiB of original history, recovering 12 exact adversarial facts
outside displayed excerpts without adding that history to execution snapshots.
These are deterministic retrieval contracts, **not semantic-quality evidence**.

Automatic model-written summaries remain a separate change. Before enabling
one, compare baseline and proposed views on held-out coding histories with
critical constraints, corrected decisions, failed checks, and exact facts.
Report serialized context bytes, tokenizer counts only where available, shared
cache-prefix bytes, retrieval decisions, task decisions, actual provider usage
and latency separately. Stable summary blocks must not repeatedly rewrite the
cache prefix; cancellation or failed persistence must retain the original view.
No claim of invisible compaction or preserved coding quality follows from
smaller payloads alone. Live inference requires explicit model/provider and
spending authorization.

### Measured foundation evidence

Baseline: `2bb01484` (v0.32.2 plus published site). Same cloud host, compiler
and build flags; no external provider calls.

- The production fixture fails on both baseline HTTP wires at the unavailable
  `search_transcript` operation. The changed executable passes both wires and
  both resumes, recovering all 12 exact facts. Search descriptions total 5,804
  bytes per run; original history is over 10 MiB. This measures a new recovery
  capability and bounded metadata, not baseline/model recall or token savings.
- Identical Unicode compaction fixture: old user and assistant messages each
  contain 159 ASCII bytes followed by `€tail`, then a latest user turn. Both
  versions save successfully before compaction. Baseline reports compaction
  success but produces invalid UTF-8, JSON serialization fails and save returns
  `-1`; changed code preserves valid UTF-8 and save returns `0`.
- Existing private-RPC verifier covers the two new method kinds: 10,400 compiled
  representative cases and seven all-input admission obligations pass. This
  proves only the admission predicate, not parsing or end-to-end execution.

The materialization RPC's production caller belongs to the separate retrieval
preset change; that change must exercise selected-source import through a real
execution snapshot on both HTTP wires before claiming end-to-end support.

`complete` in search metadata means the cursor reached the current stored
message count. It does not certify task success or complete remote content.
Whole-message recovery preserves the exact stored tool receipt, including
partial-retention notices and other result handles; it cannot reconstruct bytes
that the original tool never retained. Successful artifact copying must not be
promoted into a claim of complete source coverage.

Automatic enablement also requires a staged persistence transition. The legacy
manual runtime currently activates in-memory compact metadata before saving;
a save error is reported, but that caller does not restore the prior in-memory
boundary. Originals are retained. Do not reuse this as an automatic commit
protocol: persist the candidate before activating it, or restore the prior view
on failure, with cancellation/save-failure regression tests.
