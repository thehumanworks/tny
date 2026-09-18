Implement managed task workspace safety for #157 in feat/swarm-workspaces,
base 89bcd5918da0e225e1806813a206d007daafac0a. Read AGENTS required docs and issue
157, lead contract /Users/tomas/.tny/worktrees/5fe4c489fba3f2e0/docs/verification/swarm-delivery/contract.md.
Own src/util/task_workspace.c/.h (new), src/util/worktree.c/.h if needed, new
focused workspace tests, docs/task-workspaces.md and ADR0138. Do not modify
jobs.cpp/h, command dispatch, Makefile/Nix, SDK files or other ADRs/ledger.
Shared identity is existing job ID (32 hex) as run, integer item index as stable
task and job attempt as attempt. Core owner extends jobs DAG, lead integrates
your C API into submit/CLI after your commit. Provide clear C ABI operations to
prepare/inspect/integrate/cleanup managed per-task worktree using validated
run/task/attempt. Ownership is proven through private metadata, not arbitrary
paths supplied by callers. The lead will authorize calls through job service.
Reuse worktree helpers, avoid competing job authority. Worktrees isolate files
not privileges. Default editing isolated; shared writable only explicit policy.
Dirty/untracked launch tree requires explicit base choice or clear refusal.
Record base commit/branch/path/task provenance and final revision/diff; return
structured patch data to lead. No automatic merge on success. Explicit integrate
must detect conflicts and preserve both diffs; never destroy user state.
Cancel/failure preserves dirty trees. Remove only with proven ownership and
clean state; idempotent after crash; never adopt existing branch/path collisions.
Native non-Git/SSH/wasm behavior must be clear before side effects. Add actual Git
fixtures with two workers editing same relative path, launch checkout unchanged,
conflict, dirty/untracked refusal, collisions, repeated cleanup and foreign trees.
If safe full interface requires core glue, implement the real helper and tests,
return exact wiring; no stubs or imaginary acceptance claims. Build tests with
existing mechanisms if possible; lead owns Makefile/Nix integration. Run relevant
worktree tests and quality. Independent design review completed before work.
Commit coherent work. No push/PR/issue close/merge or further delegation. Return
SHAs/files, C interface, commands/exits, criterion mapping and remaining gaps.
