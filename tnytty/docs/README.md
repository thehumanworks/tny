# tnytty documentation

Implementation contract for **tnytty**: the tiny terminal. A C11 terminal
emulator built on the same principles as the tny harness — smallest binary,
microsecond startup, docs before code — with a headless VT core, a pty host,
kitty graphics (bundled `icat`), and a REST HTTP API that makes every
terminal session scriptable and shareable.

Do not start product code until you have read this index and the files it
names. tnytty lives in the tny monorepo
([root ADR 0045](../../docs/adr/0045-monorepo-and-tnytty.md)); the root
`AGENTS.md` governs shared conventions (quality gates, vendored libraries),
this tree governs tnytty.

## Read first

| Doc | Why |
| --- | --- |
| [product.md](product.md) | Goal, non-goals, success metrics |
| [architecture.md](architecture.md) | Headless core, seams, one event loop |
| [implementation-plan.md](implementation-plan.md) | Ordered phases and acceptance gates |
| [platforms.md](platforms.md) | macOS, Linux, Windows, iOS: what each gets and how |

## User surfaces

| Doc | Why |
| --- | --- |
| [cli.md](cli.md) | Command tree: `run`, `gui`, `serve`, `icat`, flags |
| [config.md](config.md) | The config file: location, keys, defaults |
| [http-api.md](http-api.md) | REST surface, auth, session sharing |

## Decisions

Numbered, append-only, same rules as the root
[`docs/adr/`](../../docs/adr/README.md) — but a separate sequence:

| ADR | Decision |
| --- | --- |
| [0001](adr/0001-headless-core-and-renderers.md) | The VT engine is a headless, I/O-free library; pty, API, and renderers are adapters |
| [0002](adr/0002-http-api-and-auth.md) | The HTTP API is the scripting surface; loopback is open, non-loopback requires a bearer token |
| [0003](adr/0003-kitty-graphics-and-icat.md) | Kitty graphics passes through and is recorded; `icat` is a built-in subcommand, not a separate binary |
| [0004](adr/0004-nerd-font-width-policy.md) | Private Use Area codepoints (nerd fonts) are width 1; width is computed from built-in tables, not locale `wcwidth` |
| [0005](adr/0005-native-renderer-and-macos-window.md) | The native renderer is a CPU cell rasterizer behind a window seam; macOS drives AppKit from C through the Objective-C runtime, titlebar transparent by default |
| [0006](adr/0006-split-panes-and-the-layout-tree.md) | A window holds many sessions in a binary split tree; panes share one framebuffer and one loop, and iTerm2's Cmd-D / Cmd-Shift-D are the bindings |
