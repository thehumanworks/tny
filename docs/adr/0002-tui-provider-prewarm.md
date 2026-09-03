# 0002 — The TUI pre-warms the provider's host at startup

Date: 2026-08-20
Status: accepted (amends the "no backend spawn until a turn starts" invariant). Codex references are historical: [ADR 0065](0065-codex-chatgpt-responses-backend.md) turned `codex` into a builtin profile of the native loop (no host process).

## Context

The original startup invariant said: *no backend spawn until a turn starts*.
That kept `--help`/`--version` and the first TUI paint in the microsecond
range, but it made the first interaction slow: submitting the first prompt
paid for spawning `codex app-server` (or the cursor bridge, or an ACP agent),
waiting for its ready signal, and running the protocol handshake — seconds of
dead air on what should feel like a chat.

## Decision

**The interactive TUI starts the host and initializes it as soon as the shell
paints. One-shot CLI commands stay fully lazy.**

Mechanics (`src/tui/tui_prewarm.c`):

- Right after the banner (and again after `/provider`, `/model`, `/fast`,
  `/resume`, `/new` drop the bound backend), the TUI calls
  `tui_prewarm_start`, which creates the backend object and runs its
  `connect()` — spawn + ready-wait + handshake — on a detached pthread.
- The first turn *adopts* the connected backend (`tui_prewarm_take`). If the
  warm-up is still mid-connect, take blocks on a condvar — worst case equals
  the old lazy behavior.
- **Amended 2026-08-20 (docs/adr/0004):** the thread also runs
  `create_or_resume` (thread/start, CreateAgent, session/new) against a
  resume pointer frozen at `tui_prewarm_start` — for cursor this removes a
  ~300 ms cloud round trip from the Enter path. This is safe because every
  ctx mutation that `create_or_resume` reads (model, tier, workspace dirs,
  provider, session) already drops-and-rewarms; those command paths now call
  `tui_prewarm_drop` *before* mutating, and drop waits out only the
  create_or_resume window (connect stays fully async). A pending warm-up is
  also invalidated when the resume pointer no longer matches the session.
  Backends must not *write* ctx from `create_or_resume` (cursor's model
  write-back moved to `send`).
- If the adopted host died while the shell sat idle, that now surfaces at
  `send()` instead of at `create_or_resume`: the first send on a
  freshly-adopted backend retries once through the ordinary lazy path.
- A pre-warm failure is **silent**; the error resurfaces on the lazy path at
  first prompt, exactly where it appeared before.
- Abandonment (provider switch, quit) flags the warm-up; whoever finishes
  last — the thread or the main loop — disconnects and destroys the backend.
- Applicability: codex always (spawn or `--codex-ws` attach), cursor only if
  `CURSOR_API_KEY` is set, ACP only with an `--agent` argv, openai never
  (plain per-turn HTTPS, nothing to warm). Missing-credential providers must
  fail at the first prompt with today's error, not spawn doomed processes.

## Why a thread, given the one-event-loop rule

`connect()` is blocking by design (ready-line waits up to 30 s). Making every
backend's connect a non-blocking state machine would have rewritten all four
transports. The thread runs exactly one call and hands the object back before
any turn starts; no events, fds, or session state ever cross threads. The
one event loop still owns everything that streams.

## Consequences

- The invariant is rephrased: *the CLI spawns nothing before a turn;
  the TUI is allowed to warm the selected provider after first paint.*
  `--help`/`--version` are untouched.
- Linux builds link `-pthread` (already in libSystem on macOS).
- An idle tny keeps a host process alive that previously only existed
  during/after a first turn. `/quit` tears it down.
- Host startup diagnostics had to become quiet (TNY_DEBUG-gated) because a
  background thread must never scribble on the TUI.
