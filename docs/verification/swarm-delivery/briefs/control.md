Implement core durable DAG/team foundation for #153/#155 in feat/swarm-control.
Base 89bcd5918da0e225e1806813a206d007daafac0a. Read AGENTS required docs and full
issues 153,155 via gh, and lead contract at
/Users/tomas/.tny/worktrees/5fe4c489fba3f2e0/docs/verification/swarm-delivery/contract.md.
Own src/core/jobs.cpp, jobs.h, src/cli/cmd_jobs.c, tests/integration/test_jobs.py,
new focused tests if needed, docs/jobs.md and ADR 0136. No other native files,
Makefile/Nix/shared registrations, SDK/shell changes, other ADRs or ledger.
Critical architecture: extend EXISTING jobs records and supervisor, no second
controller/execution authority. job ID = run ID, item index = stable task ID,
existing attempt/item attempt = attempt identity. Extend opt-in submit JSON
with DAG dependency indices, optional task labels/lead-worker roles and parent
session lineage captured from trusted context (never authorize by supplied ID).
Dependencies gate launch in existing supervisor. Persist claims before launch.
Retain existing batch semantics when absent. Record explicit unverified state;
execution zero/hash never becomes acceptance. Stable fingerprints/dependency
hashes/workspace revision required for safe carried outputs. Reuse retry/owner/
cleanup-unknown rules, concurrent retry and immutable attempts. On uncertain
external effects require explicit retry, never silently repeat. Validate config
cycles/duplicates/indices before side effects. Add inspectable lineage/results.
Use existing jobs CLI/tools for first compatible public slice. Design simple
CLI aliases if valuable; lead owns global dispatch/tool registration. Do not
invent deferred stubs. Worker provider/model selection must keep ceilings and
credentials private. Keep wasm refusal/SSH/embedded limitations explicit.
Add barrier provider tests A succeeds B interrupted C depends B, resume keeps A
without another request; failures block descendants; repeated/concurrent retry,
corrupt artifact, changed definitions/dependencies, cleanup uncertainty, permission
ceilings. Run relevant jobs suite and focused tests; lead runs integrated gates.
Workspace worker will add standalone util/task_workspace C API. Do NOT integrate
workspace code until lead does it; coordinate by documenting needed call point.
Lead owns shared admission and max_steps propagation after your jobs commit.
Report early design/API decisions by writing a brief local DESIGN.md if useful.
Commit coherent work, no push/PR/issue closure/merge or nested delegation. Return
SHAs, exact tests/exits, criterion mapping, remaining gaps (never claim all #153
from metadata alone), required wiring and risks. Aim substantive end-to-end DAG.
