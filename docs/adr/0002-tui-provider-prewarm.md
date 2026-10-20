# 0002 — The TUI pre-warms the provider's host at startup

Date: 2026-08-20
Status: accepted (mechanism superseded by ADRs 0053 and 0166; retained as historical context)

(amends the "no backend spawn until a turn starts" invariant). Codex references are historical: [ADR 0065](0065-codex-chatgpt-responses-backend.md) turned `codex` into a builtin profile of the native loop (no host process).

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

The original mechanism (a `src/tui/tui_prewarm.c` module with `tui_prewarm_start`/`tui_prewarm_take` called after the banner and after `/provider`, `/model`, `/fast`, `/resume`, `/new` dropped the bound backend) was removed. Interactive and one-shot turns now run in detached session runners ([ADR 0053](0053-forked-turn-isolation.md) and [ADR 0166](0166-global-sessions-and-immediate-backgrounding.md)); see those ADRs for the current startup model.

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
