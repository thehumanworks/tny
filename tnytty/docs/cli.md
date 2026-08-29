# tnytty — CLI

Noninteractive-first, like the harness: layered `--help` with examples,
`--version`, no pty or socket touched before a subcommand needs one.

```text
tnytty                      # alias for `tnytty run` (spawn $SHELL)
tnytty run [flags] [-- CMD ARGS...]
tnytty serve [flags]
tnytty icat [flags] FILE|-
tnytty --help | --version
```

## `tnytty run`

Attach the current terminal to a new session: raw mode passthrough both
ways, mirrored through the VT core so the session stays scriptable while
a human is typing in it. Exits with the child's exit code; the local
termios is always restored.

| Flag | Meaning |
| --- | --- |
| `-- CMD ARGS...` | Command to run (default `$SHELL`, else `/bin/sh`) |
| `--cols N --rows N` | Initial size (default: the attached tty's size) |
| `--listen HOST:PORT` | Also serve the HTTP API on this loop |
| `--token T` | API bearer token (see [http-api.md](http-api.md)) |

`SIGWINCH` on the attached tty resizes the session live.

## `tnytty serve`

Headless: no local tty, sessions exist only via the API.

| Flag | Meaning |
| --- | --- |
| `--listen HOST:PORT` | Bind address (default `127.0.0.1:7681`) |
| `--token T` | Bearer token; required for non-loopback binds (auto-generated and printed if omitted) |

## `tnytty icat`

Print an image inline in any kitty-graphics terminal (kitty, ghostty,
WezTerm, tnytty renderers). Reads a file or stdin (`-`). PNG is
transmitted as-is (kitty `f=100`); other formats are a clean error in
phase 1 ([ADR 0003](adr/0003-kitty-graphics-and-icat.md)).

```sh
tnytty icat photo.png
curl -s https://example.com/x.png | tnytty icat -
```

Environment: `TNYTTY_TOKEN` (API token), `SHELL` (default command),
`NO_COLOR` is irrelevant (tnytty emits the child's bytes, not its own
styling).
