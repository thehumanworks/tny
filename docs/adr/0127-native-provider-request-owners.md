# ADR 0127: Native request and retained-turn ownership

Date: 2026-09-17. Status: implementation decision; final acceptance pending.
Scope: issue #144, private native provider ownership, extending ADR 0114.
Serial 0126 belongs to independent unmerged #142 work and is untouched.

## Decision

Keep the scheduler, permissions, callbacks, retries, continuation, checkpoint
indices and persistence in C11 `openai.c`. Use three small private aggregates:

- `request_owner.cpp`: one logical POST owns inline builder buffers, temporary
  serialized message/input/schema/flat-tool/format strings, provider-view JSON,
  serialized body, secret auth header, path, provider add-ons and header slots.
  C borrows buffers through `oa_request_buffer` and strings through consuming
  `oa_request_take_string`; `buf_detach` explicitly transfers the completed body.
- The connection aggregate uniquely owns the existing C HTTP resource. C opens,
  replaces and drops it explicitly. Failed replacement leaves no connection.
- `turn_owner.cpp`: one backend allocation contains inline text, raw-body and
  tool-log buffers, parked steer and permission/custom records. Its C layout is
  the owned value, not a second set of owning pointers. C holds a borrow into
  this aggregate and uses the existing buffer API. No per-field heap wrappers,
  general pointer registry or allocation per scalar is introduced.

Pure Responses/schema translation moves from `responses.c` to `responses.cpp`,
using the existing `tny::document` and `tny::mutable_document` owners. Function
names and C linkage are unchanged; no public header, ABI/export list, provider
capability, concurrency model or platform seam changes. Existing parser/event
and custom-tool owners remain authoritative.

## Lifetime and transfer rules

The provider view is released as soon as its last builder borrower finishes,
prior to schema/header construction. Preparation also resets it defensively.
Request preparation consumes an incoming body on every outcome, permits one
attempt only (including failed attempts), and rejects reuse with -2. An auth
allocation is sized once before secret bytes are copied, never reallocated,
and wiped before release on every exit. Missing auth names/prefixes use the
configuration defaults. A non-null owner is a facade precondition; free takes
a non-null pointer to a possibly null handle. Sending an unprepared request or
using an empty connection returns a non-retryable status.

Header/path/body storage survives synchronous write, stale keep-alive reopen
and resend. Configuration/affinity headers remain synchronous caller borrows.
Cancellation/control stop prevents reconnect/send; stopping the second request
control boundary now emits the same interrupted terminal as stopping the first,
instead of reporting a retryable I/O failure. This small correction was exposed
by the real TCP RST regression test.

Pending admission first copies every borrowed metadata field into a temporary
inline record. Only after all copies succeed does it move `tools_call` and
commit the destination. Failure preserves the source and destination. The C
transition explicitly invalidates async authority on failed custom admission,
cancel and destruction. The resource-only `tools_call_release_storage` seam
reuses the existing cleanup operations; ordinary `tools_call_free` still
invalidates before calling it for existing C callers. No destructor changes a
generation, calls a user/tool/control callback, sends an RPC, restarts a tool or
waits. Pending reset asserts that async authority was already moved, consumed
or invalidated. A moved-from permission record has no call to invalidate.

Continuation keeps the existing `if (!continuing)` text-reset rule. Final tool
logs survive terminal cleanup until their consumer reads them or the next send
resets them. Parked steer is returned/consumed explicitly in C. Decode/parser
cancellation defers cleanup until active synchronous callbacks unwind, preserving
their borrowed data.

## Allocation and failure boundary

All aggregate/container/string allocation uses existing injected allocators;
JSON and buffers retain their established allocator seams. Throwing factories
and admission/preparation facades catch all exceptions. Other C entry points
perform only nonthrowing resource operations. Destruction/reset allocate nothing.
The runtime/provider OOM latches, reserved ERROR/TURN_END pair and exactly-once
settlement remain C protocols, including later rounds after recorded usage.

Copying pending metadata before moving adds bounded transient copies to make
failure atomic. Shared external registration/generation state is not duplicated.
The request view is no longer retained across a transport write/reopen, avoiding
the first-slice lifetime extension. No size, speed or peak-memory improvement is
claimed; the supervisor will measure frozen baseline and candidate states.

## Verification

`test-native-request-ownership` exercises real ownership against a deterministic
transport boundary. `test-native-lifecycle` reuses the complete allocator-enabled
provider/runtime graph, loopback HTTP/TCP and registered custom tools. It discovers
request/pending allocation ranges, asserts failure-atomic transfers, reserved OOM
settlement, queued-result lifetime, cancellation/reuse and stale replay/control
stop. `test-native-mutation` compiles private copies and requires actual behavioral
oracle failures from passing baselines. `test-native-leaks` provides the host
resource gate; sanitizer and nonsanitized lanes are separate builds.

CI and Nix cover the targets and inputs. `tests/bench/bench_requests.{c,py,mk}`
retains the optional benchmark overlay delivered separately; Nix already includes
`../tests`. Its Git-dependent comparison runs outside filtered Nix source.
See the issue #144 contract, ownership inventory, reviews and evidence for exact
commands, limits and unrun supervisor gates. The immutable initial contract is
unchanged; implementation is not whole-issue delivery.
