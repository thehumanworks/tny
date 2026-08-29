# 21 — Free `prompt` on `--image` overflow in `cmd_ask`

High (leak). From the complexity review. Independent. Scope was trimmed
from “split `cmd_ask` into stages” to the bug only; the split is
deferred (see `tasks/deferred/`) because it touches the ADR-0031
lock/detach/finalize ordering.

`src/cli/cmd_ask.c` flag parsing: on the 17th `--image` it prints
`too many --image flags (max 16)` and `return 1` without
`buf_free(&prompt)`, while the neighbouring `--output-schema` and
unknown-flag paths free it.

## Work

- Free `prompt` (and any other owned buffers at that point) before the
  overflow `return 1`.
- Add a CLI test (`tests/integration/` or `test_cli`) that 17 `--image`
  flags exits 1 with the existing message, run under ASan in the unit
  build if the CLI test harness allows it.

## Acceptance

- `tny ask --image …` ×17 prints `too many --image flags (max 16)`,
  exits 1, and does not leak under ASan.
- No other `cmd_ask` behaviour changes; `make test` stays green.
