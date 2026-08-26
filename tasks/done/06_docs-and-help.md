# 06 — Docs and layered help

Depends on 03, 04a, 04b, 05a, 05b (documents what actually shipped).

## Work

- `docs/cli.md`: `-B`/`--background` under `tny ask` — semantics (prints
  session id, turn runs detached), output shapes (plain + `--json`),
  incompatibilities (`--ephemeral`), the locked-`--resume` error, where
  output goes (`task.log`, transcript), wasm behavior. Follow the
  noninteractive-first examples convention:
  - `id=$(tny ask -B "audit the Makefile")`
  - `tny session $id` / `tny session $id --json`
  - `tny session stop $id`
  - `tny ask --resume $id "now fix it"`
  - `tny ask --resume $id --steer "drop that — check the tests instead"`
- Document steer semantics honestly (ADR decision 7a): interrupt-and-
  redirect; pending tool work is abandoned by design; bare `--resume`
  stays the guarded path.
- `help.c`: add the flag + one example to ask's layered `--help`.
- `docs/features/sessions.md`: confirm 02a/04b edits landed and read
  coherently end-to-end (status lifecycle diagram: running → done | error |
  interrupted | stale).
- Cross-link ADR 0031 from the docs pages touched.

## Acceptance

- A reader can drive the whole background flow from `docs/cli.md` alone.
- `--help` output matches the docs.
