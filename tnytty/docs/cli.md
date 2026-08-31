# tnytty — CLI

Noninteractive-first, like the harness: layered `--help` with examples,
`--version`, no pty or socket touched before a subcommand needs one.

```text
tnytty                      # alias for `tnytty run` (spawn $SHELL)
tnytty run [flags] [-- CMD ARGS...]
tnytty gui [flags] [-- CMD ARGS...]
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

## `tnytty gui`

Run a session in a **native window** — tnytty's own renderer, not a
passthrough into another terminal
([ADR 0005](adr/0005-native-renderer-and-macos-window.md)). macOS only in
this phase; every other platform exits 1 with
`gui: not supported on this platform yet`
([platforms.md](platforms.md)).

The window starts a session the same way `run` does (`$SHELL`, or the
command after `--`), sizes the grid from the window's pixels, and resizes the
session when the window resizes. A detached per-user broker owns its pty and
VT state ([ADR 0007](adr/0007-durable-session-broker.md)). Closing the window
or using Cmd-Q saves the tab/split topology and detaches the frontend; the
sessions keep running and the next `tnytty gui` reattaches them.

A window can hold **several sessions as split panes**
([ADR 0006](adr/0006-split-panes-and-the-layout-tree.md)): each pane has
its own session, its own scrollback and its own selection, and every pane
of a `--listen` window is a session in the API. A new pane runs the same
command in the focused terminal's OSC 7 working directory when known.
The window holds up to 16 tabs, each with up to 32 panes.

| Flag | Meaning |
| --- | --- |
| `-- CMD ARGS...` | Command to run (default `$SHELL`, else `/bin/sh`) |
| `--titlebar transparent\|opaque` | Titlebar style; overrides `macos-titlebar` |
| `--font NAME` | Monospaced family; overrides `font` |
| `--font-size N` | Points; overrides `font-size` |
| `--padding N` | Points around the grid; overrides `padding` |
| `--cols N --rows N` | Initial grid (default `100x30`) |
| `--listen HOST:PORT` | Ask the broker to serve the same sessions over the public HTTP API |
| `--token T` | API bearer token |

Defaults for the style flags come from
[the config file](config.md); a bad value on the command line is an
error, not a warning.

`gui --listen` configures the durable broker, so that listener remains up
with its sessions after the window closes. Repeating the same address is
idempotent; an omitted auto-generated token is retained, while an explicit
conflicting address or token is rejected rather than replacing a live
listener.

```sh
tnytty gui                                   # $SHELL, transparent titlebar
tnytty gui --titlebar opaque -- htop         # system titlebar, showing the title
tnytty gui --listen 127.0.0.1:7681 -- vim    # drive the window over HTTP
```

Keyboard: printable text, Enter/Tab/Backspace/Escape, arrows and editing
keys as CSI/SS3 (SS3 under DECCKM), Ctrl-letter as control bytes, and
Option as Meta (ESC prefix). Everything typed goes to the **focused
pane**. Programs that ask the terminal a question — cursor position
(DSR/CPR), device attributes — are answered into that pane's pty, because
in the window tnytty *is* the terminal.

Command chords are the window's and never reach the child; the bindings
follow iTerm2:

| Chord | Action |
| --- | --- |
| `Cmd-D` | Split vertically: a new pane to the right |
| `Cmd-Shift-D` | Split horizontally: a new pane below |
| `Cmd-W` | Kill and close the focused pane; closes the frontend when it is the last |
| `Cmd-T` | Create and focus a new tab |
| `Cmd-Shift-W` | Kill every session in the focused tab and close it |
| `Cmd-Shift-[` / `Cmd-Shift-]` | Focus the previous / next tab |
| `Cmd-1` … `Cmd-9` | Focus that numbered tab |
| `Cmd-Opt-←/→/↑/↓` | Move focus to the neighbouring pane in that direction (nothing happens when there is none) |
| `Cmd-[` / `Cmd-]` | Focus the previous / next pane in reading order, wrapping — always lands somewhere |
| `Cmd-C` | Copy the selection |
| `Cmd-V` | Paste into the focused pane (bracketed when it enabled mode 2004) |
| `Cmd-Q` | Save, detach every session, and quit the frontend |

Any other Command press is left to macOS, so `Cmd-M`, `Cmd-H` and friends
still work. Key bindings are not configurable yet ([config.md](config.md)).

Mouse: clicking in a pane focuses it; click-drag selects characters,
double-click a word, triple-click a line; releasing the drag copies to
the pasteboard (`copy-on-select = false` turns that off). Only one pane
holds a selection at a time — starting one clears the others. A press in
the padding, on a divider or under the traffic lights selects nothing.
The selection clears when the text under it scrolls or is overwritten.
Mouse *reporting* to the child program (SGR 1006) is phase-2 work and is
not wired up.

A one-line status bar along the bottom edge reports transient messages
("Copied 42 characters") for two seconds; `status-bar = false` hides it
and returns its row to the grid. Both are [config](config.md) keys.

Not in this phase: scrollback scrolling, kitty graphics drawn in the
window (they are still parsed, recorded and passed through), and IME /
marked text.

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
`XDG_CONFIG_HOME`/`HOME` (the [config file](config.md)),
`NO_COLOR` is irrelevant (tnytty emits the child's bytes, not its own
styling).
