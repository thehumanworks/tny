---
name: tny-live-testing
description: Run tny against the user's real provider accounts (codex subscription, CURSOR_API_KEY) and drive the real TUI in a pty. Use for "full integration" or "test against my accounts" requests, and for verifying host process lifecycle (pre-warm, adoption, shutdown, orphans).
---

# Live-testing tny against real accounts

## Credentials and binaries

- `CURSOR_API_KEY` lives in the login shell only: fetch it with
  `zsh -lc 'printf %s $CURSOR_API_KEY'`. Codex auth is `~/.codex/auth.json`.
- Agent CLIs are wrapped by shell functions and mise shims: resolve real
  paths with `zsh -lc 'whence -p codex'` and pass via `TNY_CODEX_BIN` (only
  `tny --provider codex login` runs it; turns go straight to chatgpt.com).
  The user's HOME must stay real (mise shims break under a fake HOME).

## Protecting the user's state

- `tny ask`/TUI writes `~/.tny/settings.json` (`last_provider`, per-provider
  models) via `remember_use`. **Back it up first, diff and restore after** so
  live tests don't silently flip the user's default provider.
- Run from a scratch cwd so test sessions don't pile into a real workspace's
  session list.
- Never run live tests through `build/tny` while the mutation harness is
  active — it relinks that binary with mutants. Copy it first.

## One-shot checks (cheap, per provider)

```sh
TNY_CODEX_BIN=$(zsh -lc 'whence -p codex') \
  ./tny --provider codex ask --json "Reply with exactly the word OK and nothing else."
zsh -lc './tny --provider cursor ask --json "Reply with exactly OK."'
```

Assert on the `--json` envelope: `output`, `provider`, `exit_code`.

## TUI lifecycle checks (pty)

Drive the binary under `pty.openpty()` + `TIOCSWINSZ` (see
`tests/integration/test_tui.py` for the Term/Screen classes; the Screen
emulator is the only honest way to assert what is *visible*, because the raw
byte stream keeps everything ever printed). The live pre-warm check is:

1. launch `tny --provider X` and send **no prompt**;
2. poll `ps -ax -o pid=,ppid=,command=` for the host child
   (`cursor-sdk-bridge`, the ACP agent) — it must appear within seconds of
   the banner. `--provider codex` spawns nothing (docs/adr/0065): assert
   that NO child appears and that the reply still arrives over HTTPS;
3. submit a prompt, assert the reply AND that the host pid did not change
   (adoption, not respawn);
4. `/quit`, then check for orphans with a precise `pgrep -f` on the host
   command line — the user runs unrelated agent processes (ChatGPT.app,
   codex app-servers) that a loose pgrep will flag.

## Known lifecycle trap

Version-manager shims **fork** the real host binary instead of exec'ing it;
killing only the direct child orphans the server. tny handles this with
`setpgid` + process-group kills in every host spawn/stop path — if you add a
spawn path, do the same and re-run the orphan check above.
