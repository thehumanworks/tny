**REJECT — `58e92bf`.** The three original escape paths are fixed, but the repair introduces a use-after-free, and the provider-path audit misses several allocation and crash paths. Findings below are source-verified; fresh execution limits are stated afterward.

1. **Blocker — [callbacks.c:831](/Users/tomas/projects/tny-cpp-p1fix/src/backends/cursor/callbacks.c:831): new OOM exit retains a freed pending lease.**  
   `custom_tool_take()` consumes `pending->call` whenever it returns nonzero ([custom_tools.cpp:493](/Users/tomas/projects/tny-cpp-p1fix/src/lib/custom_tools.cpp:493)). If result parsing/serialization then fails, the new return leaves `pending` linked. Emergency `cursor_callbacks_stop()` calls `custom_tool_invalidate()` on that freed pointer. The guard at line 815 has the same ownership problem when reached with sticky failure after a previous response.  
   **Invariant:** P2-I3, P2-I4. **Fix:** clear/unlink consumed leases before any fallible result processing; clean up the list node on every exit. Add async-completion → serialization-OOM → emergency-teardown sanitizer coverage.

2. **Blocker — [bridge.c:226](/Users/tomas/projects/tny-cpp-p1fix/src/backends/cursor/bridge.c:226): stderr handling crashes on allocation failure.**  
   During `cu_dispatch → cursor_bridge_pump`, failure of `xstrndup()` passes NULL to `strstr()`. The new dispatch guard runs only after this function returns. This occurs even with quiet library logging because token filtering precedes the logging check.  
   **Invariant:** P2-I3. **Fix:** use allocation-free bounded token matching, or immediately propagate failure. Fault the token-filter copy and buffered multi-line drain.

3. **Major — [openai.c:1573](/Users/tomas/projects/tny-cpp-p1fix/src/backends/openai/openai.c:1573): OpenAI/Codex still allocate inside failed callbacks and lifecycle transitions.**  
   For example, TOOL_END event-copy OOM proceeds through `control_call`, `log_toolcall`, and `session_add_tool_result`; pending-custom copies at lines 1613–1617 continue after an earlier copy fails. Request translation, reasoning retention, and persistence have similar gaps, enumerated below.  
   **Invariant:** P2-I3. **Fix:** stop between fallible owners and immediately after callbacks; do not enter transcript updates, tool execution, or persistence with sticky failure. Extend total-allocation-index sweeps beyond the four selected request faults.

4. **Major — [map.c:569](/Users/tomas/projects/tny-cpp-p1fix/src/backends/cursor/map.c:569), [cursor.c:1226](/Users/tomas/projects/tny-cpp-p1fix/src/backends/cursor/cursor.c:1226): Cursor mapping and immediate recovery still allocate after OOM.**  
   `cu_accept_frame()` treats failed parsing as an accepted frame, then `cu_on_frame()` parses again. Normalization failures also proceed into replay bookkeeping and mapping. Separately, immediate Send→ObserveRun recovery lacks the OOM guard added to delayed recovery: failed ObserveRun construction reaches a fresh diagnostic buffer.  
   **Invariant:** P2-I3. **Fix:** propagate failure through mapping before another owner is constructed; guard both recovery branches. Sweep ordinary data frames and immediate recovery construction.

5. **Major — [callbacks.c:261](/Users/tomas/projects/tny-cpp-p1fix/src/backends/cursor/callbacks.c:261): store callbacks continue allocating—and potentially mutating storage—after failure.**  
   `save_record()` constructs the envelope after failed path allocation. Directory scans continue after failed path/document allocation; `append_event()` can continue constructing and saving a record after failed payload serialization. The guard after `server->post()` is too late.  
   **Invariant:** P2-I3. **Fix:** distinguish missing/malformed records from OOM at each operation, exit scans immediately, and prevent writes after failure. Sweep get/create/update/list/delete/append paths.

6. **Major — [callbacks.c:894](/Users/tomas/projects/tny-cpp-p1fix/src/backends/cursor/callbacks.c:894): callback-thread OOM never reaches the engine’s failure state.**  
   During blocking RPCs such as active CancelRun, store callbacks run on `blocking_pump`. Allocation failure is thread-local; the pump merely exits, and `blocking_end()` joins without transferring failure to the owner thread. Reserved OOM settlement therefore is not guaranteed.  
   **Invariant:** P2-I3. **Fix:** carry an explicit failure result across the join, mark owner-thread OOM, and test allocation failures on that thread.

7. **Major — [acp_wire.c:148](/Users/tomas/projects/tny-cpp-p1fix/src/backends/acp/acp_wire.c:148), [acp_proc.c:266](/Users/tomas/projects/tny-cpp-p1fix/src/backends/acp/acp_proc.c:266): ACP has remaining intra-message and WebSocket gaps.**  
   ID serialization still allocates `"null"` after failure; agent-request handling constructs diagnostic buffers after failed ID copying. WebSocket reader-feed failure returns into wslay, which can allocate the next buffered frame before `ac_pump_reads()` checks failure. Vendored wslay allocations are excluded from the allocation override, so the counters miss them.  
   **Invariant:** P2-I3. **Fix:** stop these helpers immediately and stop WebSocket receive processing at the callback failure boundary; cover coalesced frames and non-string/non-integer IDs.

The independent allocation inventory is:

| Path | Allocations still reachable after failure |
|---|---|
| OpenAI request construction | `build_system_prompt`’s additional owners; continuation/view conversion; `jwrite_mut_val` failure followed by closing-buffer growth; Responses input/tools/text-format conversion and fallback buffer growth (`openai.c:1020–1037`, `1142–1178`). `responses.c` continues mutable JSON allocation/copy/write loops after failed allocations. |
| OpenAI request retry | Failed provider-control callback can reach `http_open`/`http_request` during stale-write recovery (`openai.c:1315–1330`); response-control failure can reach the body read before the next guard. |
| OpenAI decoded callbacks | Mutable JSON creation/copies and joined strings in reasoning retention (`315–419`); hosted-item/citation buffers and subsequent items (`435–495`). Text/event-copy failure can still reach `oa_calls_feed` or `oa_calls_set` inside the same decoded document (`events.cpp:64`, `86`). |
| OpenAI tools/finalization | `complete_tool` control/log/transcript work; pending-custom and permission string copies; preparation/save-error fallback construction; preview/background saves; steered transcript updates and batch persistence (`openai.c:1504–1509`, `1555–1617`, `1736–1792`, `1818`, `1850–2011`, `2071–2113`). `emit_turn_end` also lacks a check between preview cleanup and usage recording; failed save can reach STEER_REJECTED’s runtime text copy. |
| Cursor RPC / SDK error decoding | Reviewed header/body, error JSON, fallback, EndStream retention, and protobuf-string failure routes now unwind without another tny allocation. |
| Cursor frame mapping | Second `jparse`; replay-document copy/write, hash-vector growth, offset/run-ID copies; tool-detail fallback, signature/status buffers; later mapped fields and result diagnostics (`map.c:42–71`, `282–298`, `346–375`, `450`, `491–571`, `582–605`). |
| Cursor dispatch | Immediate recovery diagnostic allocation (`cursor.c:1226`); stderr token-filter allocation/crash (`bridge.c:225`). |
| Cursor store callbacks | Envelope after path failure; subsequent scan path/read/parse; key copy after serialization failure; checkpoint reparsing; reply growth after failed load; append payload/event construction, save, and output (`callbacks.c:261`, `406–419`, `450–472`, `502–511`, `529–572`, `620–646`). |
| ACP | ID fallback (`acp_wire.c:148`); text duplication after failed prompt-buffer growth (`198`); agent-request diagnostic buffers (`acp_events.c:270`, `279`); wslay next-frame chunk/data/queue/flatten allocations after reader-feed failure. |
| HTTP callback server | Immediate header/body/output failures now close instead of allocating fallback responses; dispatch stops before another connection. Deferred completion remains unsafe through the consumed-lease bug above. |
| Reserved runtime settlement | Reserve preparation/enqueue and resource-only cleanup remain allocation-free in the inspected core; Cursor’s dangling pending lease can prevent reaching the pair. |

**Verified OK:**

- **Original finding 1:** request-construction `-2` bypasses ordinary finalization in tool, steer, delayed-retry, and stale-read callers. The regression checks unchanged saved usage/transcript and allocation count equal to the injected index. Restoring the original finalization route would violate that count through `session_save()`.
- **Original finding 2:** HTTP-error and EndStream error parsing now distinguish OOM and suppress fallback construction. The exhaustive error-decoding regression would catch the original fallback/second-parse allocations.
- **Original finding 3:** ACP line/batch parser and retained-text failures now stop before another message. The regression would catch the original continued line/batch allocations; its healthy cases require both valid updates to survive a malformed prefix or batch.
- **Non-OOM behavior:** inspected malformed-input branches remain distinct; SDK error messages/details remain available; ACP batch processing remains intact. Eight focused existing tests passed, including malformed details, structured/plain errors, and ACP formatting/progress. Parser smoke passed.
- **Ownership/ABI:** no additional defect found in the owned-event storage or reserve replenishment. Current production exports exactly match the expected **64** names. All **729 source**, **125 additional-input**, **121 public-header/ADR**, and **56 deliverable** manifest entries match.
- Changed OpenAI auth/turn-state and Cursor stream-auth/callback-token cleanup retain their wipes. This is not a universal wiping guarantee: existing unary auth and incoming HTTP-header buffers still use ordinary frees.

**Must-fix:** findings 1–7. No additional code-level P2-I1/P2-I5 blocker identified; P2-I6 remains dependent on measurements.

The full runner aborted with exit 134. Provider fault and mutation sweeps were not independently rerun under the read-only constraints; saved matching-source passes remain historical evidence. No edits, commits, or agents.