# ADR 0192: Recoverable working context at request boundaries

Status: accepted

## Problem

The native loop could accumulate tool output for an entire autonomous turn.
The previous automatic policy ran only after eight user turns, retained four,
and summarized old user instructions to 160 bytes while omitting tool results.
It neither addressed long tool loops nor preserved arbitrary user constraints.
A mechanical summary must not pretend to identify task completion or semantics.

## Decision

At each native Chat Completions or Responses request, build the existing
protocol-repaired view, then apply a 64 KiB **serialized-message soft budget**.
When over budget, replace eligible older tool-output strings, oldest first,
with a bounded head/tail excerpt and an exact session-local transcript reference.
Keep the newest two tool-call batches verbatim. Keep all user instructions,
assistant messages, tool-call arguments and provider reasoning unchanged.
Never split or remove call/result pairs or fold synthesized protocol repairs.

The original transcript is unchanged. `read_tool_result` resolves a
`transcript:INDEX:DIGEST` reference against that session's retained tool message,
checks the digest, and returns the requested byte slice. This uses the existing
permission/tool execution path, not a new loader, filesystem path or archive.
References survive saved-session resume and execution-server snapshots, including
ephemeral sessions; they do not work in another session unless its exact matching
message happens to occupy the same index. Digests detect stale references, not
provide cryptographic authentication. Read access remains session-scoped.

Profiles that cannot call `read_tool_result` retain full output instead of
advertising unrecoverable references. The model must recover omitted evidence
before relying on it. Excerpts explicitly do not establish success. Instructions
and tool schema prefixes do not change. A newly folded output necessarily changes
that transcript position once; its reference/excerpts then remain deterministic.

Remove automatic turn-count summarization. Explicit `/compact` remains the
existing opt-in, lossy mechanical summary; existing compacted sessions continue
to honor their stored boundary. This change does not claim to repair prior
summaries or make manual compaction semantically lossless.

## Limits and tradeoffs

- This is not a tokenizer, total-request cap, model-window guarantee, semantic
  summary or proof that an agent remembers all relevant outcomes. It trades old
  output visibility for recoverability. The agent may need another tool call.
- Protected instructions, assistant text, call arguments, reasoning, the latest
  two batches, many small outputs and the independent system/tool schema may
  exceed the soft budget. Never silently drop those to satisfy a byte target.
- Full transcript persistence and the existing provider-view copy remain.
  This reduces repeated wire payload, not transcript storage or a promised peak
  memory bound. Budget accounting currently serializes the view before folding.
- No extra inference, embeddings, background process, authentication change,
  paid benchmark or new public ABI is introduced. Wasm uses the same view and
  reference reader, but its existing unsupported code execution remains unchanged.

## Evidence

See [focused evidence](../verification/working-context/evidence.md). Unit
invariants protect exact transcript/recovery, latest batches and stable resume;
a local HTTP fixture exercises long single turns and recovery through real
`run_code` on both wires. Measurements are fixture bytes, not paid token savings
or production task-success evidence.
