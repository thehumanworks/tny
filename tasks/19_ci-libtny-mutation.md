# 19 — Gate the focused libtny mutation pass in Linux CI

Medium. From the test-depth review. Independent of 15–18.

`tests/mutation/mutate.py` is function-scoped and already has
`make test-libtny-mutation` (`libtny-safety`, `libtny-fault-mutation`,
`libtny-custom-tools`). CI never runs it. Survivors in that slice can
land as long as ASan tests stay green. Full-tree `mutate.py` stays too
slow for the required `ci` aggregate.

## Work

- Invoke `make test-libtny-mutation` on Linux x86_64 CI (same lane as
  `test-libtny-tsan` / fuzz, or an adjacent job on the `ci` aggregate).
- Keep Darwin/musl/Windows/wasm off this job unless the harness is
  proven there; do not silently skip on Linux.
- Leave full `python3 tests/mutation/mutate.py` as a scheduled or
  `workflow_dispatch` job (or documented manual target) — not on every
  PR.
- Update `docs/ci.md` so “mutation tests” matches what actually runs.

## Acceptance

- A mutant that `make test-libtny-mutation` currently kills turns the
  Linux CI aggregate red if the focused tests are weakened to miss it.
- Default `make test` is unchanged (mutation is not folded into the
  unit+fixture path).
- `docs/ci.md` no longer implies the full mutate harness is a PR gate.
