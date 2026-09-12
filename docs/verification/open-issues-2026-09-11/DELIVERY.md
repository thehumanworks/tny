# Delivery checkpoint — draft, incomplete

Branch: `feat/durable-image-workflows` on `thehumanworks/tny`, based on
`b80c04b9df740c8388da03991cf4808c07e9cb50`. A16 records the user's explicit
commit/push/PR authorization. No merge, release, deployment or issue closure.

## Present in this checkpoint

- #122: requested/effective/actual image dimensions, strict destination
  preservation, local failure detail and SDK parity.
- #123: private resolved subagent launch, durable lifecycle and redacted
  actionable diagnostics, with permission ceilings preserved.
- #124 prerequisite: canonical foreground event JSONL, including backpressure.
- #126 prerequisites: truthful image-input policy and the reviewed captured
  queue/control lifecycle. Local and SSH bytes are captured once. Terminal
  failure disposes pending and unsent transcript images without later-turn leaks.
- #127: private versioned manifests/replay, owned approved execution plans,
  retained paid-artifact detail, provider metadata redaction on failure, and
  fail-closed replay lineage/destination allocation checks.

## Unfinished implementation and review gates

Jobs and exports remain in their preserved isolated worktrees at this checkpoint.
Export corrections have received focused approval. Jobs still need verified
owned-descendant cleanup, interrupted retry-snapshot recovery, a meaningful
carried-success manifest oracle, provider/tool environment compatibility and
canonical no-clobber image transaction integration. Neither worker's old passes
are final combined-tree evidence. Generated-result and selected artifact/job
preview orchestration is not implemented; ADR 0097 records the reviewed design.
The six issues and the same 49-invariant native goal remain incomplete/active.

## Evidence

`artifacts/delivery-20260912/` contains source-bound runs and fresh review records.
D002/D010 full Mac tests, D004/D013 ABI/SDKs, D007 Mac leaks and D008 eight
compiled subagent faults pass on their recorded inputs. D010 has 514 passing
unit tests. Later alias-allocation changes have focused original/fault/restored
proof; whole-tree results must be refreshed after each integration.

The candidate worktree's unchanged `make format` and `make quality` pass.
Canonical D003 failed because untracked frozen archival C probes entered the
format inventory. Those probes were not rewritten and no quality rule was
weakened. The candidate includes all delivered product/test/config/dependency
inputs and excludes only unpublished historical scratch artifacts.

Preintegration Linux full tests and real Chromium WASM backpressure/refusal
checks passed. The historical dictation attribution was wrong: all 36 dictation
cases passed; the old full suite exceeded an outer 300-second deadline. Live
Codex generation/editing succeeded (1254x1254); manual QA observed a blue square
changed to red. This is not an app preview-delivery claim or final platform proof.

## Blockers that publication does not waive

1. **I-G6/C-G6:** baseline prefixes 0030 and 0045 collide. All 88 finalized
   baseline ADR-directory filenames/bytes are unchanged. Only the user can
   resolve preservation versus global uniqueness.
2. **I-G9 host-package incident:** an export worker installed Homebrew
   ImageMagick and 14 dependencies outside its allowed scratch scope. The user
   was notified. Package/version receipts are recorded; nothing is automatically
   uninstalled. Preservation conformance remains unresolved pending disposition.
3. Required final combined behavioral, mutation, Linux analyzer/Valgrind,
   macOS, native Windows/MSYS2, browser-WASM, Nix and live-provider gates remain
   open until current input-bound evidence exists. Windows ARM emulation does
   not replace native x86_64 CI. Nix sandboxing is not yet verified. Final Linux
   stripped release size still requires measurement below 1,048,576 bytes.
4. Earlier failures, skipped rows, the historical export mutation survivor and
   the historical worker environment-log exposure remain recorded. A clean
   retained-artifact scan does not establish the scope of that earlier exposure.
5. Pushed-commit CI must be observed. No CI pass, completed goal or closed issue
   is inferred from a commit, push or draft PR.

## Publication and preservation

The PR contains product sources/tests/docs and a curated set of contracts,
original scope/baseline records, current reviews and source-bound evidence.
Large historical source copies, executable probes, VM assets, archives, raw
agent session logs and unreviewed evidence remain on disk, unstaged. They are
not discarded or silently certified safe. Historical Markdown references may
therefore point to local retained artifacts not included in this draft; the
reconciliation inventory and source hashes preserve their identity. No secret
or generated live image is intentionally included in the PR.
