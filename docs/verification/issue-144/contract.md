# Issue #144 — native provider request and pending-turn ownership

Initial discovery: 2026-09-17T08:34:10.273721+00:00.
Issue: https://github.com/thehumanworks/tny/issues/144 (open, no comments at retrieval).
Base: `4e760be09908d61e23029486e1c9a3b91fbfcbbe` (`origin/main`). Branch: `feat/cpp-native-request-144`.
The existing main checkout was clean; previous migration branch and all other
worktrees remain untouched. #142 remains independent ongoing checkpoint work;
this slice is based on current main, not an unmerged checkpoint patch.

## Goal and boundary

Make native request/turn resource ownership explicit using private C++20 while
preserving the C scheduler, C facade/public ABI, provider protocol, tool policy,
control callbacks and persistence semantics. Scope follows issue #144 exactly:
request documents/serialized bodies, HTTP connection lifetime, retained per-turn
text/error buffers, permission/custom pending records. Reuse parser/event owners.
No concurrency redesign, new provider capability, session migration, MCP rewrite
or tnytty changes. A new scoped ADR authorizes the ownership boundary; historical
ADRs and this initial contract are immutable.

## Verification invariants and acceptance

V01: Inventory each owned resource and its creation, transfer, borrowed views,
invalidation and destruction. Exactly one owner per resource; safe partial setup,
idempotent reset; never duplicate wrapper/raw ownership. Synchronous callback
views cannot outlive their owner, and reentrant cancellation cannot free an active
borrowed view.
V02: Destructors only release storage/resources. No RPC, user callback, tool
restart or unbounded wait. Custom-tool invalidation/generation, permission
decisions and connection close/reopen remain explicit operations.
V03: All new allocations route through tny's injectable allocator; exceptions
stop at every C entry boundary. Active-turn OOM emits exactly one reserved
ERROR/TURN_END pair with no allocation during settlement. Retained results remain
valid until consumed; failure of a pending transfer does not double-free or replay.
V04: Responses and Chat Completions fixtures cover success, retries before and
after text, partial/error bodies, cancellation at all wait states, permission
denial, late custom completion, and detach/restart. Preserve backoff/stall
deadlines, provider affinity and consumed-tool checkpoint index.
V05: Discover and sweep allocation indices at request creation and each pending
transition, including permission-to-custom ownership transfer. Check every
injected index and allocation-free OOM settlement, not arbitrary prefix counts.
V06: Repeat connect/turn/cancel/retry under ASan/UBSan and host leak/resource
counts. Actual behavioral reset/transfer, pending lifetime and replay-protection
mutants must fail runtime oracles from a passing baseline; compile failures do
not count as killed mutants.
V07: `make test`, `make quality`, native provider/parser/runtime/custom-tool,
fault and ABI suites pass. Run supported native/wasm fixtures. Integrate new
targets and all inputs into maintained CI/Nix; disclose any unavailable host lane
and inspect remote CI on the delivered revision rather than claiming it ran.
V08: Same-host baseline/candidate request construction, mock TTFT, startup,
memory and allocation measurements, with fixture/toolchain/revision identifiers.
Shipped artifacts strictly <6,000,000 bytes; report runtime dependencies separately.
No unsupported speedup or complete memory-safety claim.
V09: Independent first-slice and final reviews, each with findings and dispositions.
No self-review substituted for independent review. Reviewers do not implement.
V10: Final ADR, user-facing documentation, exact tested-source hashes and evidence
ledger in a focused PR. Commit/push remote feature branch, create PR referencing
`Closes #144`, confirm remote HEAD and clean local tree. Do not merge main or close
the issue directly. Reconcile every invariant with actual proof before completion.

## Initial resource inventory

- `start_post_mode`: serialized body, auth/path buffers, provider-extra header
  array; body/header borrows last through synchronous HTTP write and one stale
  keep-alive reopen. Auth storage is wiped before release.
- `build_request_chat` / `build_request_rsp`: body/system buffers, mutable provider
  view, serialized message/input/schema/flat-tools/format strings. View owns JSON
  children; session/context/tool environment remain borrowed.
- `oa_impl.conn`: connect/prewarm or request creates; retry/error/disconnect/cancel/
  destroy closes; normal completion may preserve keep-alive.
- `oa_impl.text`, `rawbody`, `toolcall_log`, `steer`: retained step/turn storage,
  continuation retry preserves text, terminal paths return parked steer explicitly.
- `pending_perm`: owned id/original/effective/control strings and parsed tools_call;
  decision is C scheduling state. Successful admission transfers once; reject,
  cancel, OOM and destruction release once.
- `pending_custom`: owned metadata and parsed tools_call shared async handle;
  admission copies borrowed strings then transfers tools_call, permission-to-custom
  must not invalidate the transferred generation; explicit cancellation invalidates.
- Existing `sse`, `oa_decoder`, `oa_callset` remain their sole existing owners.
- `unsent_preview`, callback user data and configuration/session references remain
  borrowed with their existing validity/consumption rules.

## Execution ledger

Discovery complete. Implementation, tests, reviews, benchmarks and delivery pending.
Logs are task-scoped under `$HOME/.cache/tny-issue-144/`; only sanitized evidence is
committed. Fixtures use throwaway contexts and dummy credentials, never live keys.
