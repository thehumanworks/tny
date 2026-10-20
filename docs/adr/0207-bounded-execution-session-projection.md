# 0207 — History-independent execution session snapshots

Date: 2026-10-20
Status: accepted

## Goal and measure

Keep tools usable throughout long coding sessions without discarding history.
Measure execution snapshot bytes independently of retained transcript size and
prove tools still run after a compacted saved transcript exceeds 8 MiB.

## Decision

The private execution-state session envelope carries `projection: 1`. Its `doc`
contains an empty `messages` outbox and only the live fields nested tool/session
helpers need: `task`, `swarm_cap`, `swarm_definition`, `team_runs`, `team_receipts`,
`team_cursors`, and `acp_pending_context`. Existing session identity, task body,
lifecycle fields, grants, in-memory result handles and image admission state
remain unchanged. Owner transcript, compaction summary, skill injections,
extension audit, status and other history are neither copied nor transported.

The execution server has no provider inference loop. Folded `transcript:`
result handles are resolved lazily through a bounded owner read RPC (at most
64 KiB per range, base64 on the private wire for exact byte preservation). The
owner still validates index and digest. Transcript reads are serial barriers
inside `tools.parallel`; ordinary result-file reads remain parallel. Admission,
permissions, hooks and post-processing run once through the existing path.
No transcript is reopened from stale disk or copied wholesale for recovery. Swarm activation/history selection and team context
injection run in the owner before tool execution. Subagent/job result inspection
opens the addressed child session separately. Registered custom callbacks are
already unsupported across this fresh-executable boundary; MCP callbacks retain
the existing owner RPC. The exact projection is a checked contract: new nested
helpers that need another field must extend it deliberately and test that field.

Child messages are append-only context deliveries, not a replacement transcript.
Deltas cannot set/remove `messages` or mutate omitted owner fields. Existing
append suffix merging preserves concurrent owner context, and publication is
acknowledged only after the owner save. Unknown projection versions fail closed.

An unchanged session delta skips the full owner-document clone. Changed deltas
still stage an atomic copy and may serialize the complete document for durable
publication; that cost remains proportional to history. The 8 MiB transport cap
bounds the incoming execution state, not the full merged durable document.
Active metadata, configured task bodies, grants and ephemeral blobs retain their
existing size/entry bounds and can still exceed transport limits. This is not an
unlimited-state claim, a history deletion, a bigger frame cap or a new store.

Native Chat, Responses, and verified ACP use the same seam. Parallel read workers
also receive the projection. Wasm remains explicitly unsupported for fresh
execution processes. The public C ABI and on-disk session schema are unchanged.

## Verification

- `execution_state_suite`: a 12 MiB owner transcript plus audit remains outside
  identical small/large snapshots; empty delta retains the document pointer;
  a child context append persists with all old messages intact. Invalid history
  replacements fail atomically; existing permission/image/ACK tests remain.
- `test_execution_snapshot.py`: a real 10 MiB compacted saved session runs code
  plus a nested file read across two resumes on both APIs. File effects happen
  once per invocation, and original transcript/audit data remains exact. The
  lazy reader also preserves raw split-UTF-8 ranges for base64 recovery while
  refusing those incomplete ranges in text mode. The
  unchanged baseline fails both APIs at the old snapshot ceiling.
- `tests/bench/bench_execution_snapshot.py` compiles baseline and candidate
  production encoders with identical flags and shared source files. The corpus
  has 10/100/1000 messages of 16 KiB each. It reports wire bytes and encoder CPU,
  not tokens, provider cache hits, whole-cell latency or general coding quality.

Measured on Linux x86_64, GCC 14.2 `-O2`: session-state JSON changed from
164,543 / 1,641,713 / 16,413,413 bytes to 429 / 429 / 429 bytes. The 1000-message
encoder fell from approximately 27 ms CPU to below 0.01 ms in the recorded run.
These are synthetic transport measurements; full durable writes still retain
and serialize history. [Raw fixture evidence](../verification/execution-code-mode/snapshot-benchmark.json)
records compiler, shared-source hashes and per-size samples. Repeat with
`--baseline REV --output /tmp/snapshot.json`.
