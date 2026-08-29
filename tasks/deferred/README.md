# Deferred tasks

Not picked up by `examples/scripting/implement_tasks.sh` (it globs
`tasks/[0-9]*.md` only). Kept for reference:

- 18 local-native-tools tests — broad, no bug behind it; split when a
  local tool regresses.
- 22 / 23 / 24 — pure refactors (openai `run_one_tool`, extension action
  table, cursor alias helper); revisit when extending those files.
- The full refactors originally in 20 (`tui_command` table) and 21
  (`cmd_ask` stage split); the leak fixes were kept as 20/21.
- 09 and 11 were folded into 08.
