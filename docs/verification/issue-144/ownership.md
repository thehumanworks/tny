# Issue #144 ownership inventory

The immutable `contract.initial.md` is the entire acceptance scope. Implementation
now covers every listed request, retained-buffer and pending resource. Behavioral
control remains C; final source-bound acceptance is recorded separately.

| Resource | Sole owner / creation | Borrow or transfer | Reset/release |
| --- | --- | --- | --- |
| HTTP connection | `oa_connection_owner`, existing `http_open` | `oa_connection_get` lends synchronous transport access | C explicitly replaces/drops; failed open clears; destructor closes only |
| Request aggregate | one `oa_request_new` per logical POST | local POST stack owns handle across callbacks and both attempts | all success/failure/control exits free once |
| Builder body/system/image buffers | inline `build_buffer` array in request | `oa_request_buffer` lends actual `buf_t`; detach moves completed body | early scratch reset after use; aggregate cleans partial builds |
| Provider JSON view and children | existing session builder transfers into request's `mutable_document` | builders borrow children | release immediately after serialization/translation, before schema/headers; prepare defensively releases |
| Message/input/schema/flat/format strings | request inline array of existing `c_string` owners | take consumes new result and returns borrow | slot reset immediately after use; destructor handles partial setup |
| Translation source/output JSON | existing document owners in `responses.cpp` | local explicit raw borrows; serialized output transfers to caller | stack destruction on each exit; no raw JSON owner competes |
| Serialized HTTP body | request `c_string`, consumed at prepare | HTTP writes borrow immutable bytes | every prepare outcome consumes; repeated attempt rejected; request destruction |
| Auth header | request secret member allocates final size once | fixed header array borrows; stable on reopen | complete allocation wiped before free, including failed prepare |
| Path/add-on headers/header array | injected path string, four owned add-ons, 20 inline slots | synchronous write/reopen/write | request destruction; maximum slots includes terminator |
| Config/session/affinity headers | external context and POST stack | synchronous borrow through callbacks/write/reopen | existing external lifetime; connection replacement does not revoke |
| Text/rawbody/toolcall log | inline buffers in `oa_turn_storage` | C parser/runtime/consumer borrows | C retains continuation text and terminal tool logs; raw body freed after callbacks; aggregate final destruction |
| Parked steer | turn aggregate | explicit C transcript append or STEER_REJECTED event | setter transfers ownership; explicit terminal return before clear; no destructor callback |
| Permission metadata/call | inline permission record | all borrowed metadata copied before parsed call is moved in | failure leaves source unchanged; C invalidates where needed then resource-only reset |
| Custom metadata/call/async provider lease | inline custom record plus existing custom-tool state | copy borrowed permission metadata before moving call; queued result retained in existing custom owner until take | C explicitly invalidates on failure/cancel; take consumes once; record reset never invalidates |
| Local admission call | existing C parsed `tools_call` | moves only after metadata copies succeed; source is zeroed | C `tools_call_free` after success is empty; failure frees original once |
| SSE/decoder/tool fragments/runtime events | existing parser/event owners | existing synchronous or retained views | unchanged owner rules; parser_active defers cancellation until callback returns |
| Session/config/tool environment/unsent preview | external owners | original borrowed references | original persistence/consumption rules; no competing owner |

No owner destructor publishes a result, persists, invalidates a generation,
restarts work, issues an RPC or invokes a callback. Pending reset is allocation
free and requires async authority to have already been explicitly consumed,
moved or invalidated. Repeated empty reset/free is safe.

The runtime fault fixture uses actual embedding configuration (`library_mode`)
and full injected C/C++ objects, not a partly instrumented unit graph. The
request-owner fixture's aggregate counter does not count plain C buffers/JSON;
ASan/UBSan and the separate host leak gate complement that counter. A passing
counter alone is not a whole-process memory-leak proof.

## Post-review corrections and reentrancy boundary

Resource-only teardown now rejects a live custom lease with an always-on abort
in `tools_call_release_storage`, before any storage release, including NDEBUG
builds. It does not invalidate a generation. `oa_pending_reset` invokes that
seam first; C transitions remain responsible for consuming/moving/invalidating.

A turn-open latch now makes terminal emission idempotent for direct cancellation
at provider-request control, including the first request and the stale reopened
second edge. It is restored with a successful checkpoint restore and cleared on
ordinary terminal or emergency settlement. Already-cancelled retry entry skips
request construction/control. A -2 precondition send failure is terminal and
nonretryable without setting the OOM latch; ordinary cancellation returns success
with one interrupted terminal.

The allocation sweep now covers **both embedding and actual CLI configuration**,
with canonical throwaway HOME/cwd and dummy skill metadata. Bounded null/partial
allocation guards protect the reached skill discovery/parser paths. Failed setup
preserves the recorded session, submits no failed POST and admits a recovery turn.

These guarantees do not establish whole-program callback reentrancy. Direct
backend cancellation from `complete_tool`, a `note_repairs` event or the
STEER_REJECTED callback remains untested here and can revoke borrowed pending,
connection or steer storage. Public runtime cancellation deferral and the tested
`parser_active` decode boundary retain their established guarantees; no wider
concurrency or reentrancy contract is claimed.
