# 0169 — Desktop companion: effort picker, working folder, Markdown replies and a proven turn status

Date: 2026-09-23
Status: experimental; extends [0166](0166-slint-desktop-companion.md) and [0168](0168-desktop-model-picker-and-visual-language.md)

## Context

Four problems in the Slint desktop companion:

- No way to set reasoning effort. ADR 0168 left it out of the picker.
- The working folder was fixed at launch (`TNY_GUI_CWD` or the launch directory).
- tny's replies were shown as raw Markdown: code fences, `**`, `#` and table
  pipes appeared as typed.
- The note under the user's message said "Sending…" until the whole turn
  finished. `ask --events=jsonl` emits nothing while a tool runs (a local
  check showed `tool_start` arriving only when a 20-second tool call
  returned), so a delivered prompt looked stuck for the entire turn. A stream
  that ended without `turn_end` also left the reply on "Streaming…" for good.
  The status line hid every phase behind a fixed "Thinking…".

The user asked for machine-checked guarantees that the UI shows only valid
states and transitions.

## Decision

**Effort picker beside the model picker.** It offers "Default" (no
`--effort`, so the CLI's env/settings/provider precedence applies) plus:

- the selected model's catalog `efforts` when the catalog lists them;
- nothing else when it lists an empty set (the model takes no effort);
- otherwise the CLI's generic levels `off light medium high xhigh max`.

Catalog tokens must match `[a-z0-9_-]{1,32}` without a leading `-`. Any
change of provider, model, catalog or opened chat re-clamps the choice to
what is offered, and a pick outside that set is ignored. The choice becomes
a leading `--effort` after `--provider/--model`, and the bridge rejects
invalid values before spawning. The effort popup loads the catalog the same
way the model picker does, so no request is made at startup.

**Working folder in the composer** (`in <folder> ▾`). The user types an
absolute path or one starting with `~`, or picks a folder used earlier in
this window. The path is canonicalized and must be an existing directory.

tny sessions belong to their workspace: `--resume` from another folder fails
with "no session … for this workspace". So a chat cannot move. A valid change
starts a new chat in the new folder, and the old chat stays under Recent for
its folder. The popup says this before the switch.

A change is refused while a turn runs or while tools run over SSH. Afterwards
the bridge (`--cwd`), the label, the `@` file index and the Recent list are
all recomputed for the new folder. Results that arrive for the previous
folder are dropped. A relative `TNY_GUI_BINARY` is resolved against the
launch directory, so it does not follow folder changes.

**Markdown replies.** `gui/src/markdown.rs` groups a reply into blocks, one
line at a time: headings, paragraphs, list items, quotes, rules, pipe tables
and fenced code.

- Inline emphasis, code spans, strikethrough and links go through Slint's
  `StyledText::from_markdown`, which supports only that inline subset.
- HTML is escaped, so it shows as text. Images show as links.
- Code and tables are shown verbatim in the platform's monospace family,
  resolved once through fontique (already linked by Slint).
- A parse failure falls back to plain text for that block.
- Links are displayed but never followed; reply text is untrusted display data.
- The user's own messages are shown as typed.
- While streaming, the reply is re-rendered at most every 80ms, and once more
  at the end.

**One state machine for turn status** (`gui/src/turn.rs`). The note under
the user's message, the note under the reply and the status line are all
derived from `TurnMachine`:

- **User message:** "Sending…" until the prompt is written and stdin closed.
  The bridge's new `on_delivered` callback, or any CLI event, moves it to
  "Sent". When the turn finishes it clears if the saved session holds the
  message, or reads "Unconfirmed · inspect saved session".
- **Reply:** "Streaming…" while text arrives. A failed `turn_end` gives
  "Stopped before finishing"; a stream that ends without `turn_end` gives
  "Incomplete · the stream ended early".
- **Status line:** names the live phase and the elapsed time, updated every
  second: "Waiting for the model… · 12s", "Running a tool… · 1m 04s",
  "Writing…", "Saving…".

The existing classification of how a finished turn reconciles with its saved
session is kept unchanged as `turn::outcome`. Tool names and details are
still not displayed.

**Lean 4 proofs** (`gui/proofs`, Lean 4.30.0, core only):

- `Turn.lean`
  - "Sending…" is shown iff the prompt has not reached tny, in every
    reachable state.
  - A finished process always settles all notes.
  - Stale-generation events are no-ops, and turns cannot overlap.
  - The user note only moves forward.
  - A message is shown as saved only with proof from the saved session.
- `Markdown.lean`, for any line classifier:
  - Blocks concatenate back to the reply exactly: no line dropped,
    duplicated or reordered.
  - Every block is non-empty.
  - Fences are opaque, and an unclosed streaming fence is one code block.
- `Effort.lean`
  - The chosen effort is always one the selection offers.
  - Unoffered picks are ignored, and re-clamping keeps supported choices.
  - `--effort X` is emitted iff an effort is chosen, and X is always a
    validated token.
- `Workdir.lean`
  - The bridge `--cwd` and the label equal the folder.
  - The applied index, the Recent list and the open session always belong
    to the folder.
  - The folder is frozen during a turn; invalid or busy changes are no-ops.

The finite transition functions are exported by `lake exe export` to
`gui/proofs/golden/*.tsv`. The Rust tests replay every row against
`TurnMachine::step`, `turn::outcome`, the label functions,
`markdown::action` and `Workdir::step`. Tables cover every state satisfying
each machine's proven invariant. Every reachable state satisfies it, so
agreement on those rows covers all reachable behaviour.

The proofs cover the transition functions and derived labels, not Slint
rendering, CLI behaviour, the Rust line classifier's string matching (which
the Markdown theorems quantify over) or the effort picker's Rust code. For
that code, a randomized Rust test replays the same invariant over the real
`Picker`.

## Consequences

- The GUI's allowlist is unchanged. `ask` gains an optional leading
  `--effort`.
- Cargo gains a direct `fontique = "=0.11.1"` dependency, the version Slint
  already links. The monospace lookup costs 6,400 bytes stripped (measured by
  stubbing it out).
- Footprint (Linux x86-64, `cargo build --release --locked`, `strip
  --strip-unneeded` copy, baseline built from the parent commit):
  20,703,520 → 21,917,784 bytes (+1,214,264, +5.9%). Most of it is Slint's
  `StyledText` and Markdown code, which the binary now uses. `ldd` is
  unchanged.
- CI gains a `proofs` job (`leanprover/lean-action`, `lake build`, then
  regenerating the golden tables and failing on any diff).
- Changing folders cannot continue a chat. That is the CLI's
  workspace-scoped session model, stated in the UI rather than worked around.
- The CLI emits `tool_start` late for in-process turns; this ADR does not
  change the CLI.

## Verification

- `cd gui/proofs && lake build`: all theorems check, with no `sorry` and no
  `native_decide`. Finite tables use `decide +kernel`.
- `lake exe export golden` regenerates identical tables.
- `cd gui && cargo test --locked`: 70 tests, including the golden replays, a
  4,000-case randomized Markdown round trip, `StyledText` acceptance of
  escaped inline sources, and bridge fake-executable tests. Those cover
  `--effort` ordering and pre-spawn rejection, catalog effort parsing, and a
  delivery signal that fires before a silent tool phase and never fires when
  stdin is closed early.
- A fake `tny` drove the debug build under Xvfb with synthetic input, and the
  screenshots confirmed:
  - formatted headings, emphasis, inline code, fenced code with a language
    caption, nested lists, quotes, an aligned table and a rule;
  - "Sent" plus "Waiting for the model… · 0s", then "Running a tool… · 3s",
    during a silent tool phase;
  - `--cwd ws2 --provider codex --model gpt-6-astra --effort high` reaching
    the CLI;
  - a folder change that starts a new chat, switches Recent to the new
    folder's sessions and lists the old folder under "Used in this window".
- No live provider turn was run.
