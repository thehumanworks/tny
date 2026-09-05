# ADR 0072: Ephemeral quick ask from the Zsh line editor

- **Status:** Accepted
- **Date:** 2026-09-05

## Context and alternatives

A terminal user wants a disposable answer with little more typing than the
question itself. ADR 0020 already defines the lifetime; convenient input is
missing. Ordinary shell parsing gives apostrophes, question marks, dollar
signs, backticks, and pipes meanings that natural-language prompts rarely want.

| Option | Benefit | Cost |
| --- | --- | --- |
| Alias/function: `q 'prompt'` | Familiar, portable, short | Requires quoting; shell expansions happen before tny sees the text; short names can collide |
| Function/alias: `? prompt` | Matches the proposed prefix | `?` is a glob; `noglob` still allows substitutions and quote parsing |
| Intercept Enter for `? ` | Can capture literal text | Replaces accept-line behavior and interacts with plugins/history hooks |
| Explicit Zsh widget binding | Literal edit buffer; no extra command word or quoting | Zsh-specific; a new key sequence to learn |
| Leave an ephemeral tny TUI open | Existing implementation, easy follow-ups | Switches away from the ordinary shell; context lasts until exit |

## Decision

Ship opt-in `share/tny/tny.zsh` through `make install`. Source it after shell
plugins/keymap setup. **Ctrl-X, then a** invokes `tny-ask` in emacs, vi insert,
and vi command maps. Release Ctrl before typing `a`. Ctrl-A was rejected
because the target workstation uses it as tmux's prefix. Stock Zsh 5.9 leaves
Ctrl-X followed by `a` unbound; Ctrl-X is an established prefix for extended
editor commands. The widget mechanism is conventional; this particular binding is tny's choice, not a cross-shell
standard. Users can rebind the named widget with `bindkey`.

The widget pipes the literal `BUFFER` through quoted builtin `printf` into
`tny ask --ephemeral --stdin`. It never evaluates the prompt, puts it in argv
or a temporary file, calls accept-line, or adds it to history. Enter and the
selected editing mode remain unchanged. Blank input does nothing. A secondary
prompt is refused because its previously parsed input lies outside BUFFER.

Output streams in the foreground and Ctrl-C interrupts. Success clears the
buffer. Failure/interruption keeps the text and cursor for retry; Ctrl-C at
the restored prompt discards it. Shell options are localized with
`emulate -L zsh`. Noninteractive sourcing is inert; repeat sourcing registers
the same widget without stacking wrappers.

`TNY_BIN` optionally selects one executable path. `TNY_ASK_FLAGS` is a Zsh
array of global options before `ask`. Defaults use `tny` on PATH and its
normal provider/model configuration, working directory, and permissions.

## Consequences

- No C, new runtime dependency, provider logic, or binary startup change.
- Prompts bypass normal Zsh history and preexec hooks. Terminal scrollback,
  recorders, and provider retention remain outside this guarantee. Ephemeral
  is a conversation-storage mode; tools retain their ordinary permissions.
- The default key binding replaces any binding for this sequence; source
  order and explicit `bindkey` calls control customization.
- **wasm:** the widget requires native Zsh and is unavailable in the browser
  terminal. The underlying ephemeral CLI retains its existing wasm behavior.
  Bash/Fish retain the ordinary CLI; no shell parser emulation is added.
- PTY tests cover literal multiline text, metacharacters, flags, paths with
  spaces, cwd, both editing styles, unchanged Enter, history, empty/secondary
  prompts, missing executable, errors, Ctrl-C, and repeat sourcing. Packaging
  tests check the installed script.

## Primary references

- [Zsh widgets, BUFFER, bindkey, and display management](https://zsh.sourceforge.io/Doc/Release/Zsh-Line-Editor.html)
- [Zsh filename generation and substitutions](https://zsh.sourceforge.io/Doc/Release/Expansion.html)
- [Bash bind and READLINE_LINE](https://www.gnu.org/software/bash/manual/html_node/Bash-Builtins.html)

## Verification

The original checks below verified emitted bytes. Final visible-row
preservation is now covered by [ADR 0073](0073-quick-ask-preserves-rendered-output.md),
which fixes the multiline-prompt redraw bug they missed.

On macOS with Zsh 5.9:

- `make test-shell-quick-ask`: nine PTY tests pass, including cursor restoration
  and a pause longer than `KEYTIMEOUT=1` between Ctrl-X and `a`.
- `make quality`: passes; the existing GCC analyzer reports its Darwin skip.
- `make test`: the full unit/protocol/integration suite passes in the isolated
  fixture environment described below. Final binding and packaging changes
  also pass `make test-shell-quick-ask test-install-prefix`.
- Live existing Codex account, model `gpt-5.6-luna`: both direct JSON ask and
  vi-mode Ctrl-X then a return `42` for `19 + 23`, with `ephemeral:true` and
  `session_id:""`. Isolated homes contain no saved `session.json`; widget input
  does not enter shell history. Temporary credential copies are removed.
- The final shortcut also passes through a real tmux client with the user's
  Ctrl-A prefix and full managed Zsh startup: live `gpt-5.6-luna` returns
  `42`, `ephemeral:true`, and an empty session ID. The question is absent from
  shell history. The separately owned tmux test server is stopped afterward.
- The installed script matches the repository source byte for byte. The
  workstation's global Mise config owns the startup block; fresh Zsh loads
  the expected binding and model without changing the main keymap.

The broad fixture suite must run without the workstation's `TNY_TOOLS`
override. Mise shims can reintroduce that setting after `env -u TNY_TOOLS`, so
validation removes the override and uses the resolved tool directories on
PATH without the shims. This changes the test process environment only.
