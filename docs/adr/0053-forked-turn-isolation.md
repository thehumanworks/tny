# 0053 — Every turn runs in a detached session-runner process

Date: 2026-09-01
Status: accepted (generalizes 0031; adds the IPC channel 0031 decision 7a
deferred; relates to 0002/0004 pre-warm, 0011/0013 steering, 0017 wasm
parity, 0020 ephemeral sessions, 0041 wait)

## Context

Agents ran in the process of the caller. A crashed terminal, a closed SSH
connection, or a killed `tny` took the in-flight turn with it: the native
loop lost its request mid-stream, and host backends (cursor bridge, codex
app-server, ACP agents) died with the caller's process group, losing tool
work that may have run for minutes. ADR 0031 fixed this for `tny ask -B`
only — its detached child survives anything that happens to the launcher —
but foreground `ask` and every interactive TUI turn kept the old failure
mode. 0031 also named the missing piece: live observation and steering of a
detached turn "needs an IPC channel into the detached child and stays a
future ADR."

The obvious off-the-shelf answer, bundling a terminal multiplexer, is out:
tmux is ~1 MiB of C on its own, owns the terminal, and would double the
binary. tny's answer must fit the existing invariants — one event loop,
`tny_poll` for every blocking wait, sub-1-MiB stripped binary on Linux.

## Decision

**Every turn — interactive and noninteractive — executes in a forked,
`setsid()`-detached *session runner* process. The calling process is a thin
client that renders events streamed over a per-session Unix-domain
socket.** The runner is ADR 0031's background child, generalized: it owns
the backend, the engine, MCP servers, the writer flock, and every
`session.json` write; it finalizes `status`/`exit_code`/`result` and exits
whether or not a client is still listening. A dead client is a rendering
loss, never an agent loss.

1. **One new file per session: `<dir>/sock`.** The runner binds an
   `AF_UNIX`/`SOCK_STREAM` listener at the session directory next to
   `lock`/`pid`/`task.log` (0600, unlinked on exit; a stale socket from a
   SIGKILL is unlinked at the next bind). A home directory too deep for
   `sun_path` falls back to the tmux-style per-uid runtime dir
   `$TMPDIR/tny-<uid>/<id>.sock` (0700, ownership-checked, never
   chmod-fixed); if even that fails, the surface degrades to in-process
   with a printed one-liner. The parent binds **before**
   forking, so a client connect never races the child's startup. The wire
   is newline-delimited JSON both ways, reusing the normalized event
   vocabulary (`text_delta`, `tool_start`, `permission`, `turn_end`, …)
   plus a small op set: `turn`, `steer`, `cancel`, `perm`, `end`,
   `detach`. Lines are capped (1 MiB); an oversized or unparseable line
   drops that client, never the runner.
2. **Two runner modes, one implementation.**
   - **once** (`tny ask`): run a single turn, finalize, exit. With `-B`
     the prompt is carried across the fork and no client ever attaches —
     byte-for-byte ADR 0031 behavior, plus a live socket observers may
     attach to. Foreground `ask` forks first and sends the `turn` op after
     reading stdin, preserving 0004's connect/stdin overlap (the runner
     connects while the parent drains the pipe).
   - **serve** (TUI): the runner connects and `create_or_resume`s at
     startup — it **is** the pre-warm now (0002/0004 amended: a process,
     not a thread) — then runs turns as `turn` ops arrive, keeping the
     engine, host process, and host conversation alive across turns
     exactly as the in-process TUI did. It exits on `end`, or when the
     client vanishes and no turn is active.
3. **Crash semantics.** The runner ignores client death (`SIGPIPE`
   ignored; EOF marks the client detached). A turn in flight runs to
   completion, finalizes into `session.json`, checkpoints partials on the
   0031 cadence, mirrors its rendered stream into `task.log` (the 0031
   observability trail, now for every turn), and then exits (once mode,
   or serve mode with no client).
   `tny resume <id>` / `--resume` then continues the conversation from the
   stored transcript or host pointer. Liveness stays the flock probe; the
   stop path stays `tny session stop` group-signalling — the runner is a
   group leader and must not double-fork anything out of its group.
4. **`tny session attach <id>`** connects to a live runner's socket and
   streams: a `hello` (pid, provider, model, turn state), a `snapshot` of
   the accumulated turn output, then live events. Ctrl-C detaches and
   leaves the turn running; cancelling stays `tny session stop`. Attach is
   how a crashed or backgrounded turn is watched; it works for `-B`
   children and orphaned TUI turns alike.
5. **Permissions cross the wire.** yolo resolves inside the runner
   (silent allow, as today). In ask/auto modes the runner forwards
   `permission` to the attached client and blocks that decision on the
   `perm` reply; a client that detaches with a decision pending is a
   deny. `tny ask`'s never-blocks contract is preserved: its client
   answers immediately by mode.
6. **Steering is finally live.** The client `steer` op calls
   `tny_engine_steer` inside the runner; a refusal is surfaced as the
   existing `steer_rejected` event, which the TUI already re-queues
   (0011/0013). Interrupt-and-redirect (`--resume --steer`) is unchanged
   and still runs pre-fork in the parent.
7. **Single-writer discipline is unchanged in shape, wider in scope.**
   Once mode inherits the exclusive flock across the fork (0031 decision
   4) and releases it at finalize; a serve runner holds it for its whole
   lifetime — it is the session's sole writer while alive, and `tny
   resume`'s parent now only probes liveness instead of taking the lock
   itself (its runner would otherwise conflict with it across the fork
   boundary). Every turn now records `status`/`exit_code`/
   `result` — foreground and TUI turns included, closing 0031's "old
   sessions lack them" split for new activity. While a serve runner is
   alive it is the sole transcript writer; the TUI treats its in-memory
   session as a read replica, re-opens it after every `turn_end`, and
   routes session-mutating commands through the runner (`end`, compact)
   or through a runner restart (provider/model/effort/workspace changes
   already dropped the pre-warm before mutating ctx; dropping the runner
   is the same call site).
8. **Host registry.** A serve runner backing a visible TUI publishes
   `~/.tny/codex-host.json` exactly as the in-process TUI did; once
   runners never publish (0031 decision 8 — invisible processes are not
   attach targets). Discovery-attach inside a runner stays allowed and
   never sets the child pid.
9. **Scope.** `tny ask` and the TUI. `tny acp` (the ACP *server*) keeps
   in-process turns: its lifecycle belongs to the ACP client on the other
   end of stdio, and a detached runner with a dead stdio peer serves
   nobody. libtny embedders own their process model (0023/0037); the ABI
   is unchanged.
10. **In-process remains for three cases, all deliberate.** wasm: no
    `fork(2)` in the browser; `ask`/TUI keep the pre-0053 paths, `-B`
    keeps its clean error (0031 decision 10), and `session attach`
    reports no live runner (CI-enforced parity, 0017). `--ephemeral`:
    nothing durable exists to survive for and there is no session
    directory to serve from, so ephemeral turns stay in-process (0020).
    `TNY_ISOLATE=0`: a diagnostic escape hatch on native, not a supported
    mode. The in-process code therefore stays compiled everywhere;
    runner.c carries the `#ifdef __EMSCRIPTEN__` clean-error stubs (the
    extensions.c host-OS-seam pattern this ADR sanctions).
11. **No tmux, no daemon.** There is no long-lived broker: runners are
    per-session, exist only while a session has work or a warm host, and
    the registry of "what is attachable" is the session store itself
    (`sock` present + flock probe). The whole layer is plain
    fork/poll/AF_UNIX inside the existing seams; the Linux size gate
    **tightens from 1.5 MiB to 1 MiB** to pin the claim.

## Consequences

- `<dir>/sock` joins the session file layout; `docs/features/sessions.md`
  documents it. Readers treat a socket that refuses connections as "no
  live runner" (stale after SIGKILL until the next bind unlinks it).
- The parent's exit code for foreground `ask` still reports the turn
  (0/130/2), now relayed from the runner's `turn_end`; `-B` keeps the
  launch-only contract (0031 decision 2).
- MCP servers, spawned hosts, extension hosts, and shell tools are the
  runner's children: `session stop`'s group signal reaches everything, and
  a TUI crash orphans nothing (the serve runner notices EOF and winds
  down).
- The TUI pre-warm thread is gone on native (the serve runner replaces
  it); 0002's ctx-mutation rules now read "drop the runner first". wasm
  keeps the thread.
- Two engine-loop consumers remain in-process by design (`tny acp`,
  libtny); their callers own crash semantics.
- A second observer client (attach) sees the same broadcast stream; ops
  from any client of the same user are honored. The socket lives in the
  user's `~/.tny` with 0600 modes — same trust boundary as the session
  files beside it.
- Foreground turns now leave `status:"done"`/`result` in `session.json`,
  so `tny session <id> --json | jq .result` works for every completed
  turn, not just backgrounded ones.
- Known limits (recorded, not fixed): Python extension hooks live inside
  the runner, so a runner restart (provider/model/effort/workspace
  switch) replays extension lifecycle events the in-process TUI would
  have coalesced; `/rename` and `/compact` restart an idle runner rather
  than crossing the wire; a title set mid-turn can be clobbered by the
  turn's finalize. `session attach` is an observer — permission prompts
  are answered by the owning client (or by mode when none is attached),
  never by an attach.
