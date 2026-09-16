# C++ series delivery contract

Created: 2026-09-16, before implementation.
Baseline: `1d8ad71d66c06c726b3c5b35e367fec678031e85`; clean integration worktree.
Platform: macOS arm64; Apple clang 21.0.0; Python 3.14.7; Node 26.8.2.
Scope authority: user requested delivery of #137 -> #138 -> #139 using GPT-6 Astra low/high subagents, separate worktrees for parallel work, verified merges to local main and cleanup after completion.
Native goal: none (`get_goal` confirmed). Active tool rules permit creation only on an explicit goal request; this ordinary delivery request does not authorize one. That higher-priority authorization rule takes precedence over the skill's default goal-linked mechanism. The complete invariant set below remains binding; no implementation or verification scope is reduced.
Workflow: verification-contract and worktrees; root coordinates all review, ADR serials, integration, hosted checks, local-main merge and cleanup. Agents cannot spawn children or merge main. Existing unrelated worktrees and live sessions are excluded.
The issue snapshot below is the complete normative specification. Its plan steps and required tests are requirements, not optional suggestions. Each P*-I* row retains its exact pass criteria. Deliver local main only when all original gates pass. PRs may supply hosted platform proof; remote main is not to be merged or pushed without separate authorization.

## Outcome and requirements

- R1/P1-I1..P1-I6: [#137 parser and mixed build contract](../cpp-phase-1/contract.md).
- R2/P2-I1..P2-I6: [#138 runtime and async lifetime contract](../cpp-phase-2/contract.md).
- R3/P3-I1..P3-I6: [#139 runner and jobs resource contract](../cpp-phase-3/contract.md).
- R4/S-I1: every original requirement passes on combined delivered source; final startup compared with pre-series baseline meets same absolute/relative thresholds.
- R5/S-I2: use GPT-6 Astra high for complex implementation/reviews, low for bounded runbooks; separate worktrees, one writer each, no nested agents.
- R6/S-I3: merge completed agent branches into local main; remove only clean task-owned worktrees after integrated gates pass, preserve other worktrees and live processes. Retain branches. No remote-main publication implied.

## Work sequence and ownership

Root owns this series record, per-phase contracts/evidence, source manifests, ADR number allocation, benchmark execution, hosted checks, integration and local-main delivery. Phase 1 permits independent parser and build infrastructure branches, with separate worktrees and explicit files. Phase 2 starts only after phase 1 integrated gates pass; phase 3 follows phase 2. Review agents are read-only.

## Checks

Each phase C0-C6 is required. Final reconciliation checks all 18 phase invariants plus S-I1/S-I2/S-I3 against integrated main. Required native targets: Linux glibc x86_64/aarch64, musl x86_64/aarch64, macOS arm64, Windows MSYS; shared library supported macOS arm64/Linux glibc only. Emscripten node/browser fixtures and actual browser smoke required. Linux TSan/libFuzzer and Nix/CI evidence cannot be replaced by macOS compile proof.

Build/quality/behavior: make test, quality, leaks or valgrind, test-abi, test-sdks, test-libtny-fault, test-libtny-fault-sanitize, test-libtny-fuzz-smoke, phase-specific mutation/fuzz/integration targets, new parser fuzz and portable smoke. Capture output and exit status. First check baselines; actual final full gates must be warning/error-free. Source hashes exclude volatile outputs and evidence themselves. No blanket warning suppression or hidden platform skips.

Performance: reference host baseline worktree /Users/tomas/projects/tny-cpp-baseline at 1d8ad71; baseline and candidate use same compiler/flags/data. >=100 help/version observations per binary across 3 alternating batches, p50<5ms and added p50<=max(0.25ms,10%); >=20 PTY first-prompt samples p50<10ms and added p50<=max(0.5ms,10%); local-mock TTFT tui/ask-stdin 20 runs each; parser and queue corpus throughput/peak memory <=10% regression. Report raw data, size, dynamic dependencies, allocations/resource counts and wasm/glue. Freeze any revised size policy before candidate evaluation. No language performance assumption counts as measurement.

Process: contract snapshots/digests and source/ADR baseline recorded before implementation; design and first-slice independent reviews; each finalized ADR immutable in existing docs/adr, next serial allocated by root. Required externally hosted proof may use task feature branches/PRs explicitly required by issues, never remote-main merge. Completion requires every applicable original check PASS, no unresolved warnings/errors and all task-created worktrees cleaned after merges. Unavailable gates mean INCOMPLETE.

## Amendments

None.


## Delivery amendment — 2026-09-16

The current user request is to implement #137 -> #138 -> #139 in
`/Users/tomas/projects/tny`, with exactly one independent review using
`claude --model fable --effort medium -p <prompt>`, a single pass whose
feedback is dispositioned. This supersedes the prior multi-review/model
runbook, not the behavioral, performance, compatibility or failure gates.
Implementation helpers may implement bounded slices but may not review or
spawn further agents. The requested checkout is now on
`feat/cpp-ownership-137-139`; other existing worktrees, live processes and
local/remote main are preserved. No remote-main merge, issue closure or
release is authorized by this amendment. Deliver reviewable commits/PRs
with exact current evidence; unresolved gates remain explicitly unmet.
