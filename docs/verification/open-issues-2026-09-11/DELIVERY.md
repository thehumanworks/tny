# Delivery checkpoint — incomplete

PR [#130](https://github.com/thehumanworks/tny/pull/130) was merged by
`thehumanworks` at2026-09-12T21:02:59Z, head`f968265`; this agent did not merge it.
Current main is`4b859f8` after generated Pages publication. Follow-up branch
`fix/durable-workflow-verification` preserves that merge and contains the reviewed
runner correction plus retry-test strengthening. Follow-up publication is pending.
All six issues,39 requirements and49 invariants remain the active verification
scope. The external merge does not establish passing quality or waive open gates.

## Integrated behavior

- #122: requested/effective/actual dimensions, strict destination preservation,
  local failure detail and SDK parity.
- #123: private resolved subagents, durable lifecycle, permission ceilings and
  actionable diagnostics.
- #124: canonical events, durable jobs/batches, bounded cancellation and waits,
  native process ownership and verified producer-backed retry.
- #125: exports/contact sheets, exact approved bytes, original preservation,
  no-clobber publication, dimensions and derived lineage.
- #126: capability policy, captured-byte preview queue and generated/artifact/job
  preview with same-capture hash/byte-count validation.
- #127: private immutable manifests and approved plans, retained paid-artifact
  detail, producing-attempt provenance, replay and persistence opt-out.

## Verified checkpoint and remaining corrections

The1143-input V4 publication tree passes full quality D110. Linux GCC986144B,
Clang986840B and musl1051520B meet unchanged budgets;538 unit cases pass.
The final sandboxed V4 Nix flake passes538 C cases,63 integration groups,
Bash/Zsh and all4 check outputs. Installed wrapped/unwrapped986088B payloads
pass actual HTTPS and negative size/RUNPATH checks. Mac full integration's only
remaining fixture failure was corrected and the full72-job rerun passed; ABI43,
PythonSDK85 and TypeScriptSDK43 passed. Actual wasm/browser/backpressure checks
passed. Evidence binds each check to its own source, not later corrections.

All original mutation families are reconciled:28 dimensions/capability,8 portable
subagents plus separate M123.7 Valgrind,9 manifest,13 preview,8 export variants,
31 historical jobs intents and14 event intents. Earlier compile errors, timeouts,
survivors and stale-restoration attempts remain visible. The approved retry oracle
change9cc4d099 is locally integrated but not yet committed.

Hosted f968265 SDK matrix passes. Main CI Linux x86/ARM, quality, wasm, Valgrind,
fuzz, TSAN, musl and tnytty lanes pass; Mac remains running at this checkpoint.
Two real failures prevent merge:

1. Windows MSYS default noacl does not enforce requested private modes, and
   pathname-based retained-directory operations can use a replacement parent.
   The independent native filesystem design is approved under ADR0105/A27;
   first primitive is frozen in an isolated worktree. Its Mac/Linux78 checks
   and ARM Windows guest132 checks pass, but an automatic content filter stopped
   the required independent source reviewer. No caller rollout has occurred;
   the required review checkpoint is unmet. Actual corrected product flows and
   native x64 hosted units/jobs remain required; the failed hosted unit step
   prevented the jobs step from running.
2. Nix x86 exposes an early-release runner shutdown race. ADR0104/A26's reviewed
   correction holds the writer through final save/log/socket cleanup and refreshes
   resumed CLI/TUI state under ownership. New sessions publish before fork so
   the parent retains correct saved-state knowledge. Ordinary regressions fail
   on original code and pass on the candidate. The correction is integrated
   locally: Mac/Linux544 units, affected integrations and quality pass; integrated
   ABI/SDK/leaks pass. Full corrected Nix/WASM checks and publication are pending.

Exactly four real provider requests verified generation, edit and a native typed
preview whose next vision request used the captured edited bytes. Independent
pixel decoding agreed with the provider answer. This does not claim live socket
or job-selector coverage. Credentials were read only and remained runtime-only.

Final integration, source/ADR preservation, affected mutation binding and the
complete pushed-head platform matrix remain mandatory. Two user dispositions
remain unanswered: baseline duplicate ADR0030/0045 versus immutable filenames,
and the previously recorded out-of-scope Homebrew installation. No waiver,
uninstall or consent is inferred. One terminated task mutant remains a zombie
under the shared verification container's PID1, with no live descendants;
container teardown must respect other active verification work.

Use the [issue ledger](issue-ledger.md), [contract](contract.md),
[evidence](evidence.md) and current
[remaining gates](artifacts/review-merge-20260912/reconciliation/remaining-gates.json).
Historical successful checks do not establish the final corrected state.
