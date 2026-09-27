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

## Native Python runtime checkpoint and independent smoke

At implementation commit `88e3d4c`, pinned static CPython runs in a separate
OS-confined cell process with native tools retained in the execution server.
The first development release binary is 5,538,568 bytes and has no dynamically
linked system-Python dependency. This is a preliminary development measurement,
not the final paired release comparison or a release claim.

An independent rerun of `build/tny-test -s code_runtime_suite` passed all ten
tests, 113 assertions. Three real production integration cases also passed:
both provider wires with actual file effects, denied ambient file access plus
JSON, and fresh globals across cells. The earlier failed initialization smoke
is not relabelled as a pass; setup was corrected before these observations.
These tests do not replace codec reentrancy/OOM checks or the full suite.

The fixture conversion worker completed, exit0 with a successful Claude JSON
result, and its scoped commits were reviewed/cherry-picked/pushed. Separate
Opus/high workers now handle source-linked Lean gates and a scoped native JSON
codec correctness review; both work in separate worktrees. Their future outputs
are not counted as verified until terminal receipts and integration checks pass.

The held-out trial aggregate was independently recomputed from 108 unique sample
records, preserving all111 generations/repairs and every failure: CPython35/36
first and final, prior-Python wording35/36 first and final, Monty35/36 first and
36/36 final. Output-token totals are9,428/9,691/10,421 respectively. This audit
is of identities/model/usage/aggregation, not a second implementation of every
task oracle or a new inference run.

## Worker interruption and preserved implementation checkpoints

The main Opus/xhigh session ended with CLI exit1 and an explicit provider-side
safeguard error at17:51; it did not report a completed migration. Its last pushed
checkpoint ise499d90. The final uncommitted changes (correct builtins-membership
control, macOS type include and corrected journal timestamp) are preserved here.
The codec review worker was terminated by signal9 before it wrote a terminal
JSON result or committed its patch; its source edits are preserved but are not
claimed verified. The independent proof worker remains separately observable.
The supervisor continues source review and authentic regression/release checks;
no model substitution, guard bypass, main merge or release publication is claimed.

## 2026-09-27 22:23 — resumed completion from observed state

Root main is clean and up to date at `88d343d`; PR #198 exists. The previous
Opus/xhigh implementation session exited with a provider safeguard error rather
than a completion result. Its work and transcript remain preserved. All hosted
checks on the last pushed `baf0edd` revision passed except the Linux full suite
and its aggregate, due to the native manifest-permission fixture linking the
public-library unsupported-Python stub. The unpushed Unicode bootstrap fix and
complete embedding/replay evidence are retained, not discarded.

A new narrowly scoped Opus/high build-fixture task runs durably in tmux
`tny-python-completion`, with the requested permission/output flags, JSON output,
stderr and atomic terminal status under `.agent/completion/`. It is fixing the
fixture's native link graph, not weakening production checks.

Earlier local full-suite failures also included an unresolved Go shim and an
incorrect comparison-only version label used for a production version test.
Completion checks use explicit installed Go/Python/Zsh paths and the real Git
version. Local GCC 16 analyzer findings are kept distinct from the already-green
GCC 14 CI lane; the final quality run uses that same GCC 14 analysis toolchain.

The final Unicode-complete production comparison binary is 6,283,208 bytes
versus the preserved 1,401,904-byte Lua baseline. Its complete language/JSON
embedding probe is 5,159,448 bytes (not the earlier bootstrap-only 4,668,312 B).
Fifty paired startup samples and fifteen paired loopback-tool samples were
repeated on the now-lightly-loaded host with the exact binaries/hash receipts.
Old loaded-host measurements remain historical, not overwritten. Generated
site metadata containing the temporary benchmark version was restored to its
committed state; published metadata will be generated from the actual release.
