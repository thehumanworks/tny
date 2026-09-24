# 0173 — Batched in-turn tool-result clearing

Date: 2026-09-24
Status: proposed

## Context

Turn-based session compaction does not reduce repeated tool output inside one
long agentic turn. In the 59-session usage sample, tool results account for
14.8 MB of saved transcript data, mostly terminal output and file reads. The
model may still need an older result, so clearing must keep a retrievable
original. Rewriting the prefix on every request would also lose prompt-cache
reuse.

## Decision

`TNY_EXP_CTX_EDIT=1` enables clearing in the native OpenAI-compatible loop.
The unset flag leaves existing request bytes and behavior unchanged. The flag
is read into `tny_ctx` once. This applies to both Responses and Chat
Completions, including native subagents. ACP clients own their context and are
not changed. It uses the same session code on native and wasm. An ephemeral
session keeps originals in the existing in-memory result store, accessible
through `read_tool_result`; a saved session writes them as private
`<session>/results/<handle>.txt` files.

Before a new model request in a turn, the preceding response's reported
`input_tokens` must exceed `TNY_EXP_CTX_EDIT_TRIGGER` (default 48,000). Results
larger than 1,024 bytes and older than the latest `TNY_EXP_CTX_EDIT_KEEP`
(default 8) tool results in that turn are eligible. Each is saved before its
content is replaced with a stub recording tool name, byte and line counts,
and the full-output location. Tool call IDs, calls, assistant text, and all
reasoning items remain in place. The changed transcript is persisted before
the next request, so resume and transcript inspection reflect what the model
saw. A failed store or session save fails the request rather than losing the
only copy of an output.

Before writing any originals, the pass estimates the affected suffix from
the earliest eligible result to the end of the transcript. It proceeds only
when the estimated bytes removed (allowing 256 bytes for each stub) reach at
least one quarter of that suffix. This favors a few large batches over edits
that invalidate substantial cached history for little gain. After a clearing
batch, the next trigger is the estimated post-clear token
count plus `TNY_EXP_CTX_EDIT_STEP` (default 32,000). The estimate subtracts
one token per four bytes removed from the preceding provider-reported input
count. A pass with no eligible results waits for another step of growth. A
retry does not repeat the pass. This groups prefix changes into batches;
the provider remains the authority for actual token usage. Each batch adds
a `context_edit` record under `session.json` `context_edits` with estimated
before/after tokens, cleared item count, and an estimated payback in later
requests, and emits a status event. With the verified gpt-6 input price ratio
(uncached 1, cached 0.1, cache write 1.25), the estimate is
`11.5 × affected_suffix_bytes / removed_bytes`. It is a pricing heuristic,
not a provider bill or a prediction of remaining turn length.

This feature is independent of model-written compaction. A compaction boundary
may hide older turns while context editing only changes eligible results in
the current turn.

## Local measurement

`python3 tests/bench/bench_ctx_edit.py` runs a 60-tool-step Responses turn
against a loopback mock; `--wire chat` repeats it on Chat Completions. It
produces 2–30 KiB file results and reports usage as one input token per four
request bytes. This measures request size and prefix stability, not live model
quality or provider cache hits.

| Metric | Responses off | Responses on | Chat off | Chat on |
| --- | ---: | ---: | ---: | ---: |
| Model requests | 61 | 61 | 61 | 61 |
| Serialized history bytes | 30,252,173 | 11,295,187 | 30,450,972 | 11,493,986 |
| Distinct historical prefixes | 1 | 6 | 1 | 6 |
| Clearing batches | 0 | 5 | 0 | 5 |

The on-flag reduced serialized history bytes by 62.7% for Responses and
62.3% for Chat. The five additional prefixes coincide with the five batches.
Estimated payback ranged from 19.6 to 27.8 later requests per batch. Per-request
sizes are reported by the script and in the worker status file. These numbers
exclude system instructions and tool schemas, which are unchanged between
runs. No live inference was run.

## Rollback

Unset `TNY_EXP_CTX_EDIT` to restore the old request path. Previously saved
stubs and their originals remain valid session history and result files.
