# ADR 0073: Preserve answer rows when quick ask returns to ZLE

- **Status:** Accepted
- **Date:** 2026-09-05
- **Amends:** [ADR 0072](0072-zsh-ephemeral-quick-ask.md)

## Problem

The quick-ask widget used `zle -I` before streaming external output, then
`zle reset-prompt` afterward. With a two-line shell prompt, the final answer
row disappeared when the prompt returned. Taller prompts could erase more
rows. This also happened when the answer already ended with a newline.

The original PTY tests asserted that answer bytes were emitted and that the
prompt returned. Both assertions passed even when the terminal subsequently
erased those answer bytes from the visible screen. Short live arithmetic
checks did not establish preservation of a multiline rendered answer.

## Decision

Use `zle -R` after streaming. `zle -I` has already invalidated the old editor
display and prepared for output. A normal refresh respects that state and
paints the new prompt at the current cursor position. `reset-prompt` invokes
redisplay, which moves back toward the old multiline prompt origin; those
cursor movements can now land inside the external command's answer.

Keep the existing output separation newline, literal stdin transport,
ephemeral flag, key binding, and failure/cancellation retry behavior. Do not
guess the prompt's height or add a variable number of blank lines. Provider
output and JSON/NDJSON data are unchanged. This is native Zsh display behavior;
the browser/wasm CLI is unaffected.

Add final-screen regressions using tmux's terminal emulator. Each case owns
a private socket, server, and temporary home. Tests inspect `capture-pane`
after the shell accepts another edit, proving the answer survived both
completion and a subsequent redraw. They cover one-, two-, and four-line
prompts, wrapped input/output at 24 columns, missing trailing newline,
failure with a retained retry buffer, and output that scrolls the screen.

tmux is a **test-only** dependency, supplied by the Linux quality job and Nix
test derivation. It is not part of the widget or the tny session runner.
Hosts without tmux report an explicit skip for these screen tests; the
original Zsh PTY tests still run.

## Evidence

At 40 columns with a two-line shell prompt, the same streamed fixture renders:

```text
Before                         After
host ~                         host ~
> What model are you?           > What model are you?
First answer line.             First answer line.
Second answer line.            Second answer line.
host ~                         FINAL OUTPUT MUST REMAIN VISIBLE.
>
                               host ~
                               >
```

- The pre-fix widget fails five of the six new screen tests. Only the
  one-line shell prompt case passes.
- The fixed widget passes all six screen tests and all nine existing PTY
  tests, including cancellation and retry/cursor restoration.
- `TNY_TEST_WIDGET=/path/to/widget.zsh` selects a candidate for the screen
  tests without changing the installed widget, enabling before/after checks.

Final validation on macOS / Zsh 5.9:

- `make test-shell-quick-ask test-install-prefix`: nine PTY tests, six
  final-screen tests, and the install-prefix test all pass.
- `make quality`, toolchain-pin checks, and Nix/CI matrix checks pass.
  The GCC analyzer retains its explicit Darwin skip; a Nix build was not run.
- Using the installed widget and existing Codex account with `gpt-5.6-luna`,
  a three-line human answer remains intact at 40 columns. The last row is
  `43217` (the result of `43000 + 217`), followed by the shell prompt.
  The complete answer remains visible after further typing and erasing that
  input. The question is absent from shell history. The owned tmux test
  server is stopped.
- The installed `~/.local/share/tny/tny.zsh` matches the source byte for byte.
  Already-open shells must source this file again to reload the function;
  new shells pick it up through the existing managed startup block.

## References

- [Zsh 5.9 reset-prompt, redisplay, and trashzle implementation](https://github.com/zsh-users/zsh/blob/zsh-5.9/Src/Zle/zle_main.c)
- [Zsh line editor display controls](https://zsh.sourceforge.io/Doc/Release/Zsh-Line-Editor.html)

The cancellation fixture synchronizes its second Ctrl-C with the restored
edit buffer, not the earlier error diagnostic. Its fake child's signal
handler uses unbuffered `os.write` and `os._exit` so an interrupt during the
`WAITING` flush cannot re-enter Python's buffered stdout. The full fixture
and screen suites, plus 20 repeated cancellation/recovery trials, pass.
