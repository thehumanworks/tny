# CPython code mode — v0.24.0 delivered

**Production uses CPython 3.14.7, not MicroPython.** The complete requested
migration is merged and the stable GitHub release is published.

## Delivery identity

- PR #198 merged into main; 45 implementation/benchmark/review checkpoints retained.
- Runtime release source: `7c40a20704256c50480f3707841601c17528b56d`.
- Annotated tag, unchanged at the verified source: `v0.24.0`.
- GitHub publication: `2026-09-27T23:20:16Z`, neither draft nor prerelease.
- Release workflow `36355208334`: success, all five platform builds and complete
  SDK registry-set validation passed before publication.
- The root checkout `~/Projects/tny` is updated on main. Its independently rebuilt
  native binary reports `0.24.0`; the implementation worktree and scratchpads are
  retained at `~/Projects/tny-python-code-mode`.

The exact runtime source passed both PR and independent main-push CI/SDK gates.
The later closeout commits change documentation/evidence and preserve generated
site updates; runtime/build/test inputs and the release tag remain unchanged.

## Verification actually completed

| Evidence | Observed result |
|---|---|
| Full local `make test` | Exit 0; 612 unit tests, 47,455 assertions; 101 integration groups, with explicit platform skips |
| Source-linked Lean gate | Exit 0; 36 C specifications, 17 overflow obligations, compiled cross-checks and 38 expected mutation rejections; runtime-selection proofs also passed |
| Exact-source PR CI / SDK | Runs 36352172108 / 36352172133 succeeded |
| Exact-source main CI / SDK | Runs 36355208347 / 36355208319 independently succeeded |
| Original generated-Python replay | 36/36 programs and 108/108 variants passed in final production CPython |
| Held-out production replay | All 333 recorded outcomes across 111 attempts reproduced, including original failures—not 333 successes |
| Published-release integrity | 37 asset downloads; all 36 payload checksums matched SHA256SUMS; all 37 GitHub API digests matched, including the manifest |
| Published Linux x86-64 glibc and musl | Both binaries report 0.24.0; each passes 17 production execution cases with one explicit wasm-only skip |
| Signed provenance | Both Linux x86-64 archive attestations verify against this repository, release.yml, refs/tags/v0.24.0 and exact source SHA; self-hosted runners denied |
| CPython license | Exact pinned license included and checked in all five CLI archives |

The actual published x86-64 binaries are 6,337,680 bytes (glibc) and 6,041,376
bytes (static musl), with no libpython dependency. The same-host paired Ares
comparison remains 6,283,208 bytes for Python versus 1,401,904 bytes for Lua;
these are different compiler/build observations, not interchangeable artifacts.

## Runtime choice and boundaries

Agent compatibility, not a binary-size target, governs this release. The corrected
MicroPython JSON facade passed the older 36-program corpus, but only 12/36
CPython-worded programs on the newer targeted compatibility replay, versus
CPython's 35/36. These are targeted runtime replays, not fresh MicroPython-specific
model generations or a universal language ranking. Full raw benchmark protocols,
failures, repairs, uncertainties and runtime measurements remain in this directory.

CPython's parser/compiler/evaluator is embedded directly; there is no Python-
subset fallback. The code-mode API deliberately restricts arbitrary imports and
ambient filesystem/network/process access. The native JSON facade and limited
frozen codec availability are documented in ADR 0179 and final-runtime-audit.md;
this is not an unrestricted Python installation. The Opus/high read-only audit
confirmed the actual runtime and found no release-blocking issue in its scope.

Lean proves the translated deterministic gates, not CPython, the OS, all C memory
safety, or stochastic model competence. The previous local Nix-GCC finding in
unchanged jobs.cpp remains disclosed; successful hosted quality does not rewrite
that local history. PyPy/RustPython were researched but not built or assigned
invented benchmark numbers. Optional npm/PyPI registry publication jobs were
skipped by the configured condition; GitHub release assets are published, but
no registry upload is claimed.

## Inspectable receipts

`data/published-release-readback.json`, `data/release-workflow.json`,
`data/main-push-gates.json`, `data/release-gates.json`,
`data/completion-receipt.json`, and `data/main-tagged-build.json` bind these claims
to actual source, outcomes and hashes. Raw provider-session transcripts remain
local under `.agent/`; no authentication files or live credentials were exported.

## Tag and release integrity scope

The tag has not been moved, and its source SHA plus the published asset digests
are pinned in the receipts. GitHub reports release immutability disabled; the
no-rewrite policy is not a claim of server-enforced immutable-release protection.
Signed provenance and all digest matches were actually verified at the recorded
readback time. No repository-wide release-protection setting was changed.
