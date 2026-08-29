# 20 — Fix the `/provider setup` `copy` leak in `tui_command`

High (correctness). From the complexity review. Independent. Scope was
trimmed from “table-ize the whole command ladder” to the bug only; the
table refactor is deferred (see `tasks/deferred/`).

`src/tui/tui_commands.c` `tui_command` duplicates the command line into
`copy` and every branch falls through to a single `free(copy)` — except
the `/provider setup` path, which `return`s early twice (the
“finish the turn first” refusal and after `tui_wizard_start`), leaking
`copy` each time.

## Work

- Make both `/provider setup` returns reach the shared `free(copy)`
  (break out of the ladder, or a `goto out`). Do not restructure other
  commands.
- Add a unit or TUI integration case that `/provider setup` during an
  active turn prints the existing “finish the turn first” message and
  that the wizard starts when idle, so the paths are exercised under
  ASan/LSan.

## Acceptance

- ASan/LSan `make test-unit` (or the TUI suite) reports no leak from
  `/provider setup` / `tui_wizard_start`.
- Command behaviour and help text are unchanged.
