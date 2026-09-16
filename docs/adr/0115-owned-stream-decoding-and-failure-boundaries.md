# ADR 0115: Owned stream decoding and failure boundaries

Date: 2026-09-16
Status: accepted
Related: issue #137; ADR 0114

## Decision

Keep request scheduling, HTTP/TLS/ACP/MCP transports, retries, tool execution,
checkpoints and transcript policy in C. Four private C++20 implementations own
SSE framing, Connect framing, tool-call fragments and Chat/Responses decoding.
The public library headers and ABI are unchanged.

SSE and Connect C facades are zero-initializable, non-owning views of lazily
allocated private state. Their opaque pointer is the sole ownership token:
callers must not copy an initialized facade. Initialization and repeated free
of an empty facade allocate nothing. Input spans and parser callbacks are
borrowed only synchronously. Connect retains its 64 MiB inbound frame limit;
encoding checks 32-bit representation and arithmetic without inventing an
outbound 64 MiB restriction. SSE receives no new universal size cap.

Tool fragments use a fixed 32-slot owned array and rebuild the exposed C views
after mutation. The views expire on mutation/reset and must never be freed by
C consumers. Preserve id-first attribution, missing/reused indices and the
anonymous-to-known-ID transition. Checkpoint ingress copies records in slot
order instead of assigning borrowed strings into owners.

The decoder retains its input yyjson document through callback dispatch. All
retained reasoning and unknown provider payload fields are copied into owned
mutable documents. A batch prepares its allocations before publishing any
callbacks; eight common actions are inline and a fault-injected vector handles
larger batches. Generated citation-string views are rebuilt after container or
small-string moves. Serialized extras are explicitly returned as caller-owned
allocator-compatible C strings.

## Failure and lifetime policy

All new object/container allocations use the existing tny allocator boundary,
including construction-failure cleanup. Immutable/mutable JSON documents and
object handles are move-only; destructors do not allocate or throw. No global
operator-new replacement or process abort is introduced. C adapters translate
allocation failures into explicit sticky OOM status, distinct from malformed
JSON. Reset is required after a sticky failure. This avoids exposing a partial
successful decode or allowing later terminal input to disguise an earlier OOM.

C consumers check the new statuses and preserve runtime emergency event
reserves. This changes resource ownership, not the existing cancellation,
scheduling, terminal settlement or retry protocols.

## Verification

Check every split position, byte-wise feeds, CRLF/comments/multiline/UTF-8/EOF,
Connect frame boundaries/trailers/keepalives, retained payloads after allocator
churn, repeated injected failures followed by successful reuse, and generated
citations through vector growth. Instrument the implementation, not only the
fuzz harness. Mutants remove the frame cap, change ID precedence, retain a
freed document view and swallow OOM; each must compile and fail its oracle.

Protocol corpus files are binary Git inputs: intentional CRLF/NUL/incomplete
wire bytes must survive Windows checkout unchanged. Source whitespace and
language-appropriate diagnostics remain enforced separately. The C++ analysis
lane checks private C++ headers without recommending ABI-changing enum
narrowing in shared C headers; those headers retain C analysis and all compiler
parse diagnostics. Standard-library paths come from the selected C++ driver.

Complete integration/platform/performance gates and the single user-authorized
independent review are required before the issue is accepted as delivered.
