# 0042 — Enforce parser/help flag alignment

Date: 2026-08-28
Status: accepted

## Context

CLI flags are parsed in several C entry points while their user contract is
rendered separately from `src/cli/help.c`. `tny session <id> --wait` and
`--timeout` were documented when they were added in [0041](0041-session-wait.md),
but no automated check would have failed if either flag had been omitted from
help. The reverse drift is equally harmful: help can advertise a spelling that
the corresponding argv parser rejects.

## Decision

`tests/integration/test_help_flags.py` statically extracts concrete long and
short flags from the global and command argv parsers, renders `tny --help` and
every `tny <command> --help`, and compares both directions:

1. A flag accepted by a parser but absent from its help is a failing test.
2. A flag printed by help but accepted by neither that command parser nor the
   leading global parser is a failing test.
3. Top-level help must list every dispatched subcommand and compatibility
   command alias.

The checker has a small, commented allowlist only for non-enumerable hidden
compatibility syntax or tokens explicitly passed through to another program.
New exceptions require a concrete justification beside the entry.

## Consequences

- Adding, removing, or renaming a CLI flag requires changing its parser and
  help in the same patch.
- `make test-help-flags` runs the contract directly, and `make test` includes
  it before the ordinary fixture integration runner.
- The checker uses only Python 3's standard library and the built `tny` binary,
  so the same contract runs in the Nix test sandbox.
