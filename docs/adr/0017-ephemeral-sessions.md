# ADR 0017: Ephemeral sessions are process-local conversation state

- **Status:** Accepted
- **Date:** 2026-08-23

## Context

`tny ask --no-save` previously skipped `session.json`, but persistence was not
one coherent mode. The interactive TUI had no equivalent, prompt history was
always appended to `~/.tny/history`, large native-loop tool results were still
written under a session `results/` directory, and host backends could create a
durable provider thread. A user therefore could not reliably start a CLI or
TUI conversation that disappeared with the process.

The conversation still has to remain multi-turn while the process is alive.
The native backend needs its transcript and result handles, and host backends
need their current remote session/thread identifier. Removing all session
state would make the interactive shell single-turn rather than ephemeral.

## Decision

`--ephemeral` is a leading global flag for every conversational surface:

```text
tny --ephemeral
tny --ephemeral ask "…"
tny --ephemeral acp
```

`tny ask --ephemeral` is also accepted after `ask`. `--no-save` remains a
compatibility alias in either position.

The mode is carried by `tny_ctx.no_save` so the persistence boundary is
central rather than reimplemented by each surface. While it is active:

- the working session document remains in memory for the life of the process;
- `session.json` is not written;
- recovery checkpoints are not written or loaded;
- large tool-result blobs use an in-memory handle store instead of
  `results/<handle>.txt`;
- TUI prompt history is process-local and neither reads nor appends
  `~/.tny/history`;
- saved sessions cannot be opened, resumed, recovered, migrated, or imported;
- JSON `ask` output reports `"ephemeral":true` and an empty `session_id`;
- the TUI prints an explicit ephemeral-mode status line.

The provider adapter applies a no-store control when the protocol defines one:

- Codex `thread/start` receives `"ephemeral":true`;
- the native OpenAI Responses wire already sends `"store":false` for every
  request and reconstructs context from tny's in-memory transcript;
- other host protocols receive no invented field. Their service or agent may
  retain data according to its own policy even though tny stores no local
  conversation artifact.

Configuration is outside the conversation boundary. Existing non-conversation
settings behavior, such as remembering the last selected provider/model, is
unchanged.

## Consequences

An ephemeral TUI remains fully multi-turn until it exits, including native-loop
tool-result range reads, but it cannot be resumed after exit. Crash recovery,
`/continue`, `/resume`, and saved prompt history are intentionally unavailable.
Large results consume process memory instead of disk, bounded by the lifetime
of the session.

The guarantee is precise: tny does not persist local conversation/session
artifacts. It is not a universal promise that every remote provider discards
request data; that depends on the provider and wire protocol and is documented
per backend.
