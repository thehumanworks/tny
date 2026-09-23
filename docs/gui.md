# Slint desktop companion (experimental)

`gui/` is a small Rust + Slint **desktop** companion to `tny`, not a new agent
runtime. The existing executable continues to own providers, tools, permissions,
sessions, SSH, images, jobs and swarms. The GUI calls an allowlisted set of CLI
operations without a shell, streams canonical `ask --events=jsonl` events and
keeps the Slint event loop free of blocking CLI work. See [ADR 0166](adr/0166-slint-desktop-companion.md),
for the model picker and visual language [ADR 0168](adr/0168-desktop-model-picker-and-visual-language.md),
and for the effort picker, working folder, Markdown replies and the proven turn
status [ADR 0169](adr/0169-desktop-effort-folder-markdown-and-verified-turn-status.md).

## Run

Requirements: Rust/Cargo, the native dependencies required by Slint's winit +
femtovg desktop backend, a graphical Linux or macOS session, and a built,
configured `tny` executable. No live account is required to build or test.

```sh
make release                   # if build/tny is not already present
cd gui
cargo test                     # fake CLI; no provider/network access
cargo run                      # from this directory, cwd defaults to repo root
cd proofs && lake build        # Lean 4 proofs of the UI state machines
```

Set `TNY_GUI_BINARY=/absolute/path/to/tny` to choose another binary and
`TNY_GUI_CWD=/absolute/workspace` to choose a workspace. Otherwise `tny` on
`PATH` is used outside the repo. `TNY_GUI_BINARY` should be absolute when
running outside the repo, since the CLI process changes directory to the
workspace. Provider authentication/configuration remain the CLI's; the GUI
does not store API keys. It inherits the CLI's effective permission mode
(default yolo) and must **not** be treated as a sandbox or a permission prompt.

## What the desktop UI does

| Surface | Behavior |
| --- | --- |
| Chat | Streams text and per-turn usage. tny's replies render as Markdown (headings, emphasis, inline code, links, lists, quotes, rules; fenced code and pipe tables in the platform monospace face); links are shown, never followed; the user's own text is shown as typed. Each turn reports its state under the messages and in the status line: the prompt reads "Sending…" only until tny has it, then "Sent"; the status line names the phase (waiting for the model, running a tool, writing, saving) with the elapsed time; a stream that ends early is marked "Incomplete", never left "Streaming…". It opens saved session transcripts and resumes idle sessions after an explicit choice of where tools run. An in-flight/unconfirmed turn blocks automatic repost. Saved running/stale/checkpointed sessions open read-only. Enter sends; Shift+Enter inserts a new line. |
| Effort | The composer's `Effort · …` picker, next to the model. "Default" passes no `--effort`, so the CLI's env/settings/provider precedence applies. It offers the selected model's catalog `efforts` when listed, nothing when the catalog lists an empty set, and otherwise the CLI's generic `off light medium high xhigh max`. The choice is re-clamped whenever the provider, model, catalog or opened chat changes, and is passed as a leading `--effort`. |
| Working folder | The composer's `in <folder>` control sets the CLI's `--cwd`. Enter an absolute path (or `~/…`) or pick a folder used earlier in this window. tny sessions belong to their folder, so switching starts a new chat there; the old chat stays under Recent for its folder. Not available while a turn runs or while tools run over SSH. `@` files and Recent follow the folder; results computed for the previous folder are dropped. |
| Model | The composer's `provider · model` picker. Providers come from `tny providers --json` (local, at startup and Refresh); unhealthy ones show "Needs setup" and cannot be picked. A provider's catalog (`tny --provider P models --json`) is fetched only when the picker needs it and cached per window. "Default" omits `--model`; an unlisted ID can be typed. The choice becomes leading `--provider/--model` on `ask`. Opening a saved chat shows the provider/model it resumes with; an explicit pick overrides that pin. |
| Draft | Optimise sends a draft by stdin and requires review before Send; Dictate records ten seconds to an editable draft. These services need their independently configured providers/microphone. |
| Completion | Clickable `/new`, `/refresh`, `/usage`, `/help`; local `@` paths and `$` skill names. Git paths honor Git excludes; non-Git scans are bounded and skip symlinks. Remote path and skill completion is deliberately disabled under SSH, not mislabeled as remote. The list is a bounded cache; Refresh rebuilds it. |
| Images | Attach an existing local image with `ask --image`; Generate writes to an explicit absolute path using `image generate`. A successful generation can be explicitly attached to the next turn. No pixels are automatically rendered/previewed. Generation may replace an existing destination and the CLI's default manifest stores the prompt; do not use a sensitive output location without reviewing `docs/images.md`. A failed finalization can leave a committed image, which the UI labels separately. |
| Where tools run | Tools panel choice between *This computer* and an *SSH host* (set before a new chat's first turn). The GUI still runs locally; tny executes workspace tools remotely. When reopening a saved session, *historical SSH details are not recoverable*; choose where tools run explicitly before submitting. |
| Swarms and messages | The composer's *Solo / Swarm of N* control enables a local-only `--swarm=N` first turn. *Follow a swarm run* takes its 32-character ID; refresh shows agents, the run and *parent/operator* mailbox capacity. *Open inbox* (parent inbox) is explicit: it marks queued messages delivered but **does not acknowledge** them. Message bodies are untrusted display data, not instructions or proof of verified work. Other participants' inboxes cannot be read by the operator. |
| Usage | Streamed turn tokens, saved session totals, workspace totals and explicit Codex allowance lookup (if configured). Missing usage is labeled unavailable, never zero. Run/worker verification remains `unverified` until checks are independently performed. |

## Visual language

One page tone throughout; hierarchy comes from type weight and lighter tones
of the same green-grey scale rather than panels, rules or background changes.
Only surfaces that float (composer, fields, popovers, tooltips) are elevated,
and only with a shadow. Interaction never changes a color: clickable elements
get a pointer cursor, and selection is shown with weight or a check mark.
Every icon-only or non-obvious control carries a tooltip that says what it does
and what it affects. Below 1100px an open tools panel takes the sidebar's
place so the conversation keeps a readable width.

## Proofs

`gui/proofs` is a Lean 4 (core only) project, pinned by `lean-toolchain`. It
models the turn/message status, Markdown block grouping, effort picker and
working-folder state, and proves their invariants (ADR 0169). `lake exe
export golden` writes the finite transition tables to `proofs/golden/`, and
`cargo test` replays every row against the Rust code the GUI runs. CI builds
the proofs and fails if the regenerated tables differ from the committed
copies. The proofs cover transition functions and labels, not rendering or
CLI behaviour.

## Measured footprint (Linux x86-64, September 23, 2026)

A local `cargo build --release --locked` produced a 29,361,592-byte
executable; `strip --strip-unneeded` on a copy measured **21,917,784 bytes**
(20,703,520 before ADR 0169's Markdown rendering, effort picker, working
folder and turn status; +5.9%; 19,903,904 before ADR 0168). The CLI is
still a separate executable. `ldd` reports fontconfig/freetype and standard
system libraries (libc, libm, libgcc_s, expat, zlib, bzip2, libpng, Brotli).
These are local measurements, not a binary-size gate or a macOS/iOS estimate.
No startup, memory, visual-frame or live-turn performance number is claimed.

Slint 1.18.1's local crate metadata declares alternative GPL-3.0-only,
Slint Royalty-free 2.0 and Slint Software 3.0 licensing. The top-level About
panel includes Slint's `AboutSlint` widget. Review the applicable license and
attribution/distribution obligations before shipping; this repository does
not grant a project license (see `LICENSE-METADATA.json`).

## Boundaries and next steps

- `ask --events=jsonl` deliberately uses an **in-process CLI turn**, not the
  normal crash-surviving detached session runner. The GUI cannot attach to,
  cancel, approve or steer that active turn. Closing the window is not an
  explicit cancellation/cleanup protocol. Permission requests use the CLI's
  effective unattended policy; users needing interactive permission decisions,
  attach/cancel, worktree management, full team control, mailbox ack, checkpoint
  recovery or exhaustive session pagination should use the TUI/CLI. Do not use
  the GUI as the sole interface for long-running sensitive work.
- Linux compilation/unit tests were run. The macOS target was not built or
  visually checked in this environment. No automated GUI interaction or live
  provider turn has been run. Verify locally before distributing either build.
- **iOS is not implemented.** The Slint visual language may guide a mobile
  layout, but this desktop app depends on launching a native `tny` child,
  desktop window decorations/size and local filesystem/SSH environment. The
  documented `libtny` ABI currently targets Linux/macOS and is not an iOS
  runtime. An iOS client would need an independently designed mobile layout,
  an authenticated remote-host service or a separately ported embedded runtime,
  secure credential and file handling, microphone/image permissions, and
  device/simulator integration tests. Do not present this Cargo build as an
  iOS app or assume desktop subprocess behavior exists on iOS.
- GUI tests use fake CLI executables and synthetic receipts; they establish
  argument construction, streaming/error behavior, parse validation and the
  local view model, **not** live provider correctness or platform support.

Related contracts: [CLI](cli.md), [sessions](features/sessions.md),
[mailboxes](team-mailbox.md), [images](images.md), [dictation](dictation.md),
[prompt optimisation](optimisation.md), and [TUI](tui.md).
