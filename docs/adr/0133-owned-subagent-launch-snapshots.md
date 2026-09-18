# ADR 0133: Owned sub-agent launch snapshots

Date: 2026-09-18. Status: accepted.

## Context and experiment

The user requested measured, incremental harness improvements, with critical
C ownership moved into maintainable private C++, small artifacts, and temporary
workspaces for agent experiments. Inspection selected the native sub-agent
launch plan: it manually owned the executable, private environment entries and
pointer array, but borrowed selectors and inherited environment strings.
Building over a live plan also discarded its allocations with `memset`.

The initial hypothesis that growable buffers could silently truncate a
credential on OOM was **rejected**: `buf_detach` already fails closed. This is
not a fix for a demonstrated production credential-truncation bug. The useful
improvement is explicit snapshot lifetime and failure-atomic replacement.
An independent read-only agent review ran in a temporary baseline worktree.
It confirmed the borrowed lifetimes, live-rebuild leak risk, unchecked argv
capacity, environment count arithmetic, and need for explicit secret wiping.

## Decision

Extend ADR 0114 to `core/subagent_plan.cpp`. Keep validation, permission gates,
child execution, cancellation, reaping and result classification in C11. The
private C facade is zero-initialized and noncopyable by contract. It exposes
borrowed argv/envp arrays and an opaque C++ owner, not standard-library types.
The public libtny ABI and CLI/tool schema are unchanged.

A plan owns **all** selector and environment strings. After build, changing or
releasing the source context, resume id or environment does not invalidate the
plan. The inputs and environment must remain stable during the synchronous
build; this does not make concurrent `setenv` safe.

The owner allocates an exact-size string block once, before copying any secret.
It does not use growable secret-bearing strings or retain moved small-string
views. Both argv and envp point into that block. Its destructor wipes the entire
block, including inherited environment values, before releasing it. A checked
argv append reserves a terminator; checked size arithmetic and the injected
vector allocator protect environment counts and allocation products.

Build a candidate before replacing a live plan. Any failure retains the old
owner and both views. A successful rebuild destroys the old owner once. Free
clears all views, is idempotent, and allocates nothing. Exceptions stop at the C
facade. Process-path failures retain their OS diagnosis; C++ construction
failures set `ENOMEM`. Every allocation uses the established fault seams.

Provider resolution, private credential carriers, token/account precedence,
permission ceilings, tool profiles, ephemeral flags and stdin prompts retain
their behavior. No new dependency, scheduler, process or thread is introduced.
The existing OS process seam remains authoritative: wasm still returns
`ENOTSUP` for launch, and SSH still rejects native sub-agent execution rather
than accidentally running tools locally. This is **not** a new swarm scheduler
or remote sub-agent implementation.

## Verification and tradeoffs

`make test-subagent-ownership` covers all token/account combinations crossed
with permission/tool profiles; maximum selectors; large credentials; empty
options; source mutation; successful replacement; every observed allocation
failure for both empty and live outputs; owner balance; idempotent,
allocation-free release; and full-block wiping before free. Only the isolated
test object redirects `secure_zero` to an observer; production has no test hook.
`make test-subagent-mutation` requires behavioral kills from compiled private
source copies. CI native, musl and MSYS ownership lanes and Nix include the new
gate. Existing native child integration tests remain the process-flow oracle.

Copying inherited environment strings costs memory and linear copy/wipe work.
That bounded-per-launch cost buys an independent lifetime; it does not authorize
keeping secrets longer than necessary or caching launch plans. Do not describe
the migration as a speed improvement without measured before/after evidence.
No size-policy relaxation was needed. Measurements and the iterative experiment
log are in [the experiment record](../verification/subagent-launch-ownership.md).
