# 12 — Fail closed if the integration runner is missing

Medium-low. From the repository-wide test audit. Independent.

`Makefile` `test` runs `tests/integration/run.sh` only if it is
executable. A mode regression or packaging loss turns the whole fixture
suite into a no-op while `make test` still exits 0.

## Work

- Make the integration runner a required prerequisite of `test` (a
  real Make dependency, or an unconditional invocation that fails if
  the file is missing or not executable).
- Confirm `nix/source.nix` still ships `tests/integration/run.sh` with
  mode `0755` so the Nix sandbox cannot hit the old silent skip.

## Acceptance

- `chmod -x tests/integration/run.sh && make test` fails.
- Removing `run.sh` fails `make test` rather than skipping fixtures.
- Default `make test` still runs the same fixture set as today.
