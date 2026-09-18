Continue your task in isolated workspace branch. Your helper commit 4c7e0d3 is
accepted as foundation, not full #157. Implement the explicit public shared
workspace-control service in NEW src/core/tools_workspace.c/.h and
src/cli/cmd_task_workspace.c only, plus docs/task-workspaces.md and focused
integration tests NEW tests/integration/test_task_workspace_control.py.
First cherry-pick e351f40 into your worktree for authoritative DAG records.
Do not edit jobs.cpp/h, helpers, core/tools.c/intercept/cli dispatch/Makefile/Nix.
Lead wires registry/dispatch/schema. Keep separate tools_workspace API supporting
inspect/integrate/cleanup (prepare stays exclusively in job scheduler).
Your trusted service must read/validate existing DAG job/item/attempt and prove
not running / cleanup known before Git. Bind cwd to authoritative job workspace,
never caller arbitrary path/session ID. Permit IDs job_workspace_inspect,
job_workspace_integrate, job_workspace_cleanup separate; read grants cannot
integrate or remove. Use per-attempt workspace helper. No job state lock across
Git; hold existing job OWNER lock to fence concurrent retry, nonblocking, then
revalidate state/attempt. Never infer authority from PID or arbitrary session ID.
Use tny_jobs service result plus confined job.json record if workspace field is
not projected; no raw secret output. Mutation operations require current caller
cwd equals recorded launch cwd and permission callback done by lead tools adapter.
Support public CLI `tny task-workspace inspect|integrate|cleanup --run ID --task N
--attempt N [--json]`, shared parse/detail/run helper for terminal interception and
all-tools. Do not expose prepare publicly or auto-integrate/cleanup. Refuse active
job or cleanup unknown (even if task itself exited), non-DAG/shared workspace,
wrong attempt, SSH/embedded/wasm before side effects. Return structured artifacts,
status/conflict/verification:unverified; dirty conflict state never accepted.
Scheduler owner is adding item workspace:{policy:"isolated",base:optional} and
persisted item.workspace metadata; if naming not stable, use existing raw item
request.workspace.policy and record root.workspace as launch cwd; helper metadata
is authoritative about Git ownership after job association check. Check your
validation against this concrete schema and document any required scheduler field.
Add actual C/API or CLI tests by temporary test-only driver if dispatch absent;
no manufactured passing source-only checks. Avoid helper-only claiming public
flow complete. Lead will run integrated CLI tests after wiring and scheduler commit.
Fix macOS leak fixture without weakening full native tests: split only the
fork-based overlap test into a separate suite, retain six nonfork cases in normal
leak suite. Document observed leaks-tool limitation; Linux needs full process
fixture. Register split requirements for lead. No push/PR/merge/issue closure or
further delegation. Commit and return SHA/API/exact tests/exits/remaining gaps.
