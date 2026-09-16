# Phase 2 ownership inventory

Integrated source: `feat/cpp-ownership-137-139` on top of the phase-1 parser
owners (ADR 0115). All public runtime/session mutations remain owner-thread and
owner-process operations except the documented cancellation and tool completion
paths. Owners use `src/util/ownership.hpp` (`tny::owned`, `tny::make_owned`,
`tny::allocator`); no second allocation-owner system is introduced.

| Boundary | Owner / borrower | Thread and reentrancy | Lifetime end / transfer |
| --- | --- | --- | --- |
| libtny runtime -> ctx, host services, registry | runtime uniquely owns; session and engine borrow | creator thread/process; host/tool callback flags reject reentry | runtime teardown closes session before dependencies |
| runtime -> session | runtime tracks unique open session; caller has opaque handle | creator thread/process; cross-thread cancel only with caller-held lifetime | session close detaches runtime link and destroys engine, permission, session state |
| session -> engine -> backend | session owns engine; engine owns prepared backend, borrows ctx/session/perm | owner loop only; prewarm transfers before dispatch | prepare adopts backend even on failure; engine teardown cancels then destroys |
| backend callback -> event | callback document/bytes borrowed synchronously; event owns retained bytes (`src/core/owned_event.cpp`) | synchronous owner loop; no callback reentry or blocking IO | copy before return; queue adopts only fully constructed event |
| engine -> queue/pending terminal/reserves | engine uniquely owns records | owner loop; callbacks cannot publish partial records | pending -> queue -> pop transfers unique ownership; free destroys remaining records |
| popped event -> public event / renderer | caller owns; getters, JSONL and renderers borrow immutable payload | existing adapter rules; no mutation of payload | explicit event_free, independent of engine/session/runtime; queue links never own popped event |
| reserved OOM pair | engine owns until consumed | owner loop; settlement must allocate zero bytes | pre-turn transactional replenishment; fixed turn-id slot edited before publication |
| pending terminal -> finalization | engine keeps the terminal private until finalization returns | owner loop | finalization OOM substitutes the reserved pair; success publishes the terminal |
| public error | C adapter owns message and error; caller adopts on return | same export affinity; failure may return status without error allocation | tny_error_free; scope conversion preserves OOM status |
| registration metadata | registry uniquely owns copied spec and inactive tombstones (`src/lib/custom_tools.cpp`); native tools and Cursor borrow | owner registration before session; unregister after session; visit callback cannot mutate registry | runtime registry destruction, with worker-safe registry state retained until last async reference |
| async call: provider handle | OpenAI tools_call.custom_call or Cursor pending.call uniquely owns a private pending handle; registry active list borrows its shared call state | owner takes/invalidates; completion worker synchronizes with registry mutex | take (1 or -1), invalidate and generic tools_call_free consume the handle; unregister/cancel/close only detach and invalidate state |
| async call: host handle | ASYNC callback transfers a distinct unique public handle sharing call state to host worker | complete may run off-owner; exactly one release after worker use; no use after release | explicit release, independently of provider handle; generation immutable |
| call -> registration / registry | call retains shared registry lifetime; registration borrowed within it | mutex protects active/completed/epoch/closing and result | last registry/worker reference; stale completion rejected before accessing registration |
| completion result -> backend | result bytes borrowed until complete returns, copied into call | completion mutex; duplicate and wrong generation rejected | take transfers malloc-compatible bytes to C backend; backend frees |
| provider emergency settlement (ADR 0117) | provider marks failure; runtime owns the settlement scope and reserved pair | owner loop; providers stop at the failed operation | resource-only cancel releases transport/process/parser storage; no RPC, tool result or persistence is constructed |
| host-service clock/notify and Cursor adapters | borrowed callback/state; no new ownership | existing callback guards and bounded pump ownership unchanged; pump failure crosses `pthread_join` as a flag | synchronous return; no new public concurrency support |

Moving an owning handle never moves its published record or byte storage; every
allocation uses the tny boundary; detaching an async call also detaches its list
link; retaining an async handle does not retain unrelated calls.

Reproducible focused checks:

```sh
make test-runtime-ownership test-libtny-fault test-libtny-fault-sanitize
make test-runtime-mutation
```

`test-libtny-fault` also builds `build/lib-fault/provider-faults`, which links
the real ACP, Cursor and OpenAI backends against the fully allocator-instrumented
object graph; `tests/integration/test_libtny_faults.py` runs its named
regressions and the whole-turn provider allocation sweeps.
