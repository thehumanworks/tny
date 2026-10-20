# 0206 — Recoverable execution output

Date: 2026-10-20
Status: accepted

## Goal

Recover compiler/test diagnostics without rerunning work or expanding default
model context to entire logs.

## Decision

Reuse session result handles and seek-bounded `read_tool_result`. A streaming
writer captures merged stdout/stderr before `run_code` head/tail truncation and
before ordinary local nested `terminal` truncation. Short unmodified results retain
identical text and create no artifact. An omitted or UTF-8-normalized preview
returns a recovery handle after the complete observed byte stream is stored.
Error classification stays at the beginning of the result. Capture does not
retry effects, extend deadlines, or imply completion after interruption.

Saved captures use a private 0600 temporary file, flushed and renamed into the
existing session results directory only at finalization. Memory stays bounded
by the preview and drain buffers, independent of long output size. Each capture
has a 64 MiB ceiling. Existing session lifetime controls retention; this is not
another persistence layer or garbage collector. A killed execution server can
leave an unpublished temporary capture, as with interrupted atomic writes;
no handle is published for it.

Ephemeral sessions never write capture data to disk. They share a 1 MiB
aggregate memory capture budget with existing result handles, below the existing
execution-state transport ceiling. Large session snapshots remain a separate
concern. Storage failure/cap exhaustion discards incomplete captures and returns
an explicit unavailable notice alongside the usual bounded preview. Execution
continues normally. Therefore recovery is lossless only within available
retention capacity, not an unbounded logging promise.

Raw bytes are not necessarily UTF-8. `read_tool_result(encoding="base64")`
returns exact byte ranges including NUL and malformed or split UTF-8. Default
text reads reject those ranges with instructions to use base64 rather than
silently truncate or corrupt transport. No public ABI or provider field changes.
The system prompt tells agents to retrieve diagnostics, not replay effects.

Shell-profile/background and remote SSH terminal contracts are unchanged. Direct Python on
wasm continues to return the existing unsupported-host error.

## Verification

The executable counterexample prints a diagnostic between two 70,000-byte
regions and increments an effect counter once. Before this change both HTTP
wires return a 65,510-byte preview without that diagnostic or any recovery
handle. Candidate verification must recover it through a later tool call and
saved-session resume while the effect counter remains one.

The focused provider suite covers both wires, nested terminal output above
512 KiB, errors/timeouts, near-limit error previews, binary conservation,
ephemeral aggregate exhaustion, storage failure, and unchanged small output.
Results and performance measurements are recorded after checks run; byte counts
are not token, billing, or task-quality measurements.

### Local evidence (2026-10-20)

Baseline production tree: PR #219 local commit `739c3b90` (the later main
changes before this work were documentation/Pages only). Baseline and candidate
use the release build with the same GCC/CPython toolchain. A fixed two-cell
fixture emits 140,026 bytes, then reads the diagnostic range if a handle exists;
the fallback reports unavailable without rerunning the command. Five runs per
wire recover 0/10 diagnostics before and 10/10 after, with an effect counter of
one and three provider requests in every run. The candidate retains all 140,026
raw bytes. Summed model-visible tool results grow only 148 bytes
(65,522 to 65,670); cumulative serialized request bytes grow 1,054 bytes
(Responses 151,775 to 152,829; Chat 151,907 to 152,961). These include the fixed
prompt/schema change, not measured token or billing savings.

Isolated local mock-loop median wall times over those five runs per wire were
Responses 60.90 to 63.19 ms and Chat 63.28 to 69.31 ms. This is small-sample local
storage/transport overhead, not remote-provider latency or an end-to-end
software-task speedup claim. The benefit demonstrated is recoverable evidence
without duplicate command effects, not faster execution of the original command.

The production execution fixture passed 40 runnable tests (one wasm-specific
clean-error test skipped on native); the ten new cases include saved-session
resume and both wire formats. The runtime unit suite passed 25 tests. The new stream unit passed all 32 assertions. Earlier core units
passed 141/142; the reused-stdin ACP test fails identically on the unchanged
baseline in this sandbox. Hosted native gates remain required before merge.
