# Python code-mode release scratchpad

The implementation agent owns `SCRATCHPAD.md`. This companion records independent
supervision, baseline measurement and final main/tag/publication work.

## 2026-09-27 — main and durable implementation setup

- Updated `/home/tomas/Projects/tny` to branch `main`, fast-forwarded to origin/main
  `d09a28762f2cc664eed8a8b11fd84ec712e11a42`; working tree clean.
- Implementation remains isolated in `/home/tomas/Projects/tny-python-code-mode`,
  branch `feat/python-code-mode-runtime-20260927`, based on the completed earlier
  benchmark `175092f`. The initial scratchpad checkpoint is `4442b72`, pushed.
- Launched Claude Code with `--model opus --effort xhigh
  --dangerously-skip-permissions --output-format json --verbose` in durable tmux
  session `tny-python-opus`. Its session metadata reports `claude-opus-5-5`.
- Full JSON output, stderr, atomic exit receipt and progress notes are in local
  `.agent/`; these account-session transcripts are intentionally excluded from Git.
- Latest published release at setup: `v0.23.1`. Expected next feature release:
  `v0.24.0`, selected by the repository's normal Conventional Commit/tag mechanism.
  A version string is not hardcoded into product files. Recheck tags before tagging.
- Release requires existing `ci` and `sdk` workflows green on the exact main
  commit; retain those gates. A tag or workflow dispatch is not proof of a
  published release. Verify actual release assets/checksums and binary version.

## 2026-09-27 — independently built Lua baseline

- Built the current main release in the parent's separate ignored
  `build/python-migration-baseline` directory with default native flags/GCC 16.2.1,
  `-j2`, and a common comparison-only version label `python-code-mode-benchmark`.
- First attempt hit the host's per-user `/tmp` quota; reran with a disk-backed
  `/var/tmp/tny-python-baseline`. No source/optimization change or test suppression.
- Build exit 0. Stripped executable: **1,401,904 bytes**. SHA256:
  `bb24562e7f7c9bb68cb30d5a571fbb2ea82d9059f48f9b71fa8c31f7a9944c08`.
- Measured 50 fresh-process samples after five warmups for version/help. Raw
  timings, linked dependencies and actual runtime-source hashes are in
  `data/lua-release-baseline.json`. These one-arm observations are not a causal
  before/after result; final paired/interleaved measurements must compare both.
- The baseline binary remains at
  `/home/tomas/Projects/tny/build/python-migration-baseline/tny` for final comparison.

## 2026-09-27 — candidate setup validation

- The implementation agent is testing actual Python language constructs and
  frozen model-generated programs, not inferring compatibility from a feature list.
- Independent inspection of the first MicroPython probe found a harness setup
  problem: enabling EXTRA_FEATURES enabled C-stack checks without a configured
  nonzero stack limit. The resulting exception occurred during runtime setup/error
  formatting. It is not a valid model-code incompatibility observation.
- An earlier diagnostic's missing input file also caused `fseek(NULL)` in the
  temporary harness. Neither setup failure is eligible as a runtime-comparison
  score. Candidate controls need to work before outcomes can be compared.
- PocketPy's allocator contract needs special care: some upstream allocation paths
  assume success or abort. A quota allocator cannot merely return NULL and then
  claim safe recovery. Sticky resource termination and lifecycle cleanup require
  actual boundary tests alongside source-linked proofs.

## Pending completion evidence

Runtime selection and implementation are still in progress. No Python migration,
production success, new tag, release publication or cross-platform compatibility
is claimed by this setup/baseline checkpoint.
