# 0004 — Cut time-to-first-token: resume on the warm-up thread, overlap ask's connect with stdin, attach one-shots to a live codex host

Date: 2026-08-20
Status: accepted (amends 0002; adds two one-shot mechanisms)

## Context

With 0002 the TUI's first prompt no longer paid host startup, but three
synchronous waits remained between "user is ready" and the provider seeing
the turn:

1. TUI Enter still paid `create_or_resume` — for cursor a CreateAgent round
   trip to their cloud (~300 ms), for codex/ACP a local RPC.
2. `tny ask` with a piped prompt read all of stdin, then connected — serial,
   although the two are independent.
3. Every codex one-shot spawned its own `codex app-server` (measured
   ~170 ms to listening for the binary alone, plus the initialize
   handshake), even when a long-lived tny TUI already had one running.

## Decision

**1. `create_or_resume` moves onto the pre-warm thread** (details recorded as
an amendment in 0002). The Enter path now performs no backend RPC at all
when a warm-up has landed.

**2. `tny ask` overlaps `connect()` with reading stdin.** When the prompt
will come from stdin, the backend is created and its `connect()` runs on a
joined pthread while the main thread drains stdin. Error reporting and exit
codes are unchanged; the empty-prompt and bad-`--resume` exits tear the
backend down so no host leaks. The argv-prompt path stays serial and
identical. This does not weaken the startup invariant: the turn has already
been requested when the overlap begins.

**3. Codex one-shots attach to a registered live host.** The spawn path
publishes `~/.tny/codex-host.json` (`{"ws":..., "pid":...}`, 0600, atomic
write) after its handshake succeeds and unpublishes it on teardown while the
file still names its pid. `cx_connect` without an explicit `--codex-ws`
resolves `TNY_CODEX_WS`, then the registry: the file is **untrusted data** —
only `ws://` on loopback is accepted, the pid must be alive — and any
attach failure falls back silently to spawning. A discovered attach never
sets the child pid, so process-group kills and reaping cannot touch a
foreign host. Explicit `--codex-ws` keeps attach-or-fail semantics.

## Measured (2026-08-20, tests/bench/bench_ttft.py against the codex mock)

| path | before | after |
| --- | --- | --- |
| TUI Enter → first output, 400 ms `thread/start` | 411 ms | 6 ms |
| `ask` piped stdin (400 ms producer, 400 ms `initialize`) | 875 ms | 449 ms |
| `ask` one-shot, attach vs spawn (python stub host) | 235 ms | 11 ms |

The real `codex app-server` takes ~170 ms just to accept connections, so
attach saves at least that plus initialize on real one-shots.

## Consequences

- The pre-warm thread's contract widened; the invariant sentence in
  AGENTS.md and the thread rules in 0002 are updated. `create_or_resume`
  implementations must not print and must not write ctx.
- An idle TUI now holds an already-created provider session, not just a
  connection. Hosts that expire idle sessions surface at `send()`, covered
  by the one-shot lazy retry.
- `~/.tny/codex-host.json` is new persistent state; a crash can leave it
  stale, which costs one failed loopback connect (instantaneous) before the
  spawn fallback.
