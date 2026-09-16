# Parser ownership inventory

The C facades are sole ownership tokens, not copyable owners. Initial zero
state and repeated reset/free are valid. Private C++ types allocate through
the existing tny allocator; no embedding-process allocator is replaced.

| Resource | Owner | Borrowers and lifetime | Release / failure |
| --- | --- | --- | --- |
| SSE input and event buffers | Private state behind `sse_parser.owner` | Synchronous framing callback only | `sse_parser_free`; allocation exceptions become sticky OOM |
| Connect frame accumulator | Private state behind `connect_decoder.owner` | Synchronous complete-frame callback only | Parser free/reset; checked 64 MiB inbound limit and sticky failure |
| Tool IDs, names, argument fragments | Fixed slots behind `oa_callset.owner` | C views until that slot is mutated or the callset is reset | Move-only state; failed assembly resets the entire exposed batch |
| Input JSON document | Local `tny::document` during decode | Borrowed event fields only during callback dispatch | Automatic release after dispatch or any exception |
| Retained reasoning, unknown fields and hosted items | `oa_decoder.owner` and owned JSON documents/strings | Serialized copies returned with C allocator ownership | Decoder reset; construction and serialization failures return OOM |
| Generated citations and decode action batch | Local owned records, inline storage plus allocator-backed overflow | Views rebuilt after storage movement; dispatch is synchronous | Automatic destruction after callback failure or successful dispatch |
| Backend decode state | OpenAI backend until terminal settlement | Parser callbacks borrow state; cancellation records intent while `parser_active` | Reset only after the parser stack unwinds; terminal cleanup also frees raw-body storage |
| Search completed items | Existing C search-response document | Copied from each event before its document is released | Existing response cleanup; parse/copy OOM is distinguished from malformed input |

Fixed tool slots do not move. Updating one slot refreshes that slot's C view;
restoration refreshes all populated slots. A future change to relocating
storage must rebuild every affected view. No caller may free those views.

Callback cancellation is tested over whole JSON, SSE feed and SSE flush.
Test-only process-wide atomic counters observe C++ owners/containers still
alive during the callback and released at terminal delivery, before backend
destruction. Those counters exclude ordinary C/yyjson allocations, which use
their separate fault and sanitizer/leak checks. Counter definitions are absent
from production builds; every C++ object in the counter test uses the same
instrumentation definition to preserve the one-definition rule.

Reproducible focused checks:

```sh
make test-parser-ownership test-parser-backend-ownership test-search-ownership
make test-parser-mutation test-parser-fuzz-smoke
```

The search test compiles the actual private C decoder with its exported
service renamed to avoid a duplicate symbol, and links the real instrumented
core. It exhausts discovered decoder allocation indexes, distinguishes invalid
JSON, and checks successful reuse. It does not substitute a mock decoder.

See the series evidence for failed runs, independent review corrections,
source identities, platform limits and final reconciliation. This inventory
alone is not evidence that every phase-one acceptance gate passed.
