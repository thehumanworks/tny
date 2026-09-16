# Phase 2 ownership inventory

Baseline inspected: `a33deda56d6b1388832d98bc3a52572bd15b4a70`.
All public runtime/session mutations remain owner-thread and owner-process
operations except the documented cancellation and tool completion paths.

| Boundary | Owner / borrower | Thread and reentrancy | Lifetime end / transfer |
| --- | --- | --- | --- |
| libtny runtime -> ctx, host services, registry | runtime uniquely owns; session and engine borrow | creator thread/process; host/tool callback flags reject reentry | runtime teardown closes session before dependencies |
| runtime -> session | runtime tracks unique open session; caller has opaque handle | creator thread/process; cross-thread cancel only with caller-held lifetime | session close detaches runtime link and destroys engine, permission, session state |
| session -> engine -> backend | session owns engine; engine owns prepared backend, borrows ctx/session/perm | owner loop only; prewarm transfers before dispatch | prepare adopts backend even on failure; engine teardown cancels then destroys |
| backend callback -> event | callback document/bytes borrowed synchronously; event owns retained bytes | synchronous owner loop; no callback reentry or blocking IO | copy before return; queue adopts only fully constructed event |
| engine -> queue/pending terminal/reserves | engine uniquely owns records | owner loop; callbacks cannot publish partial records | pending -> queue -> pop transfers unique ownership; free destroys remaining records |
| popped event -> public event / renderer | caller owns; getters, JSONL and renderers borrow immutable payload | existing adapter rules; no mutation of payload | explicit event_free, independent of engine/session/runtime; queue links never own popped event |
| reserved OOM pair | engine owns until consumed | owner loop; settlement must allocate zero bytes | pre-turn transactional replenishment; fixed turn-id slot edited before publication |
| public error | C adapter owns message and error; caller adopts on return | same export affinity; failure may return status without error allocation | tny_error_free; scope conversion preserves OOM status |
| registration metadata | registry uniquely owns copied spec and inactive tombstones; native tools and Cursor borrow | owner registration before session; unregister after session; visit callback cannot mutate registry | runtime registry destruction, with worker-safe registry state retained until last async reference |
| async call: provider handle | OpenAI tools_call.custom_call or Cursor pending.call uniquely owns a private pending handle; registry active list borrows its shared call state | owner takes/invalidates; completion worker synchronizes with registry mutex | take (1 or -1) and invalidate consume the handle; unregister/cancel/close only detach and invalidate state |
| async call: host handle | ASYNC callback transfers a distinct unique public handle sharing call state to host worker | complete may run off-owner; exactly one release after worker use; no use after release | explicit release, independently of provider handle; generation immutable |
| call -> registration / registry | call retains shared registry lifetime; registration borrowed within it | mutex protects active/completed/epoch/closing and result | last registry/worker reference; stale completion rejected before accessing registration |
| completion result -> backend | result bytes borrowed until complete returns, copied into call | completion mutex; duplicate and wrong generation rejected | take transfers malloc-compatible bytes to C backend; backend frees |
| host-service clock/notify and Cursor adapters | borrowed callback/state; no new ownership | existing callback guards and bounded pump ownership unchanged | synchronous return; no new public concurrency support |

Implementation review must check that moving an owning handle never moves its
published record or byte storage, that every allocation uses the tny boundary,
and that detaching an async call also detaches its list link. Retaining an async
handle must not retain unrelated calls. Independent reviews are coordinator-owned.
