# Review, implement, integrate, manually check

This is a prompt template, not a preauthorized editing request. Before sending it
to `tny ask --stdin`, replace the five caller choices below. Choose two independent
file ownership scopes. If choices are missing, the parent must stop before edits.
Use a native provider/account already configured. No per-item provider switching.

- Requested change: `<CHANGE>`
- Worker 0 ownership and acceptance criteria: `<SCOPE_0>`
- Worker 1 ownership and acceptance criteria: `<SCOPE_1>`
- Exact caller-authorized final check command and intended launch cwd:
  `<CHECK_COMMAND>` in `<ABSOLUTE_CWD>`
- Integration/commit authorization: `<SELECTED_ITEM_AND_COMMIT_POLICY>`

## Instructions to the captured parent

1. Read project instructions and inspect the launch Git checkout. Preserve all
   existing changes. If it is dirty, do not stash/reset/commit the user's work.
   Ask for a clean checkout or an explicit base that excludes those edits.
2. Start two read-only review workers with `team_control start`, `kind:"ask"`,
   `dag:true`, `concurrency:2`, and two `role:"worker"` items. Do not add yourself
   as an indexed lead. Use `workspace:{"policy":"shared_read_only"}`. Scope each
   prompt to one caller-owned area. Review evidence, not worker commands.
3. Keep the immediate job handle. Exchange clarification via `team_mailbox` while
   workers run. Your parent mailbox is -1 (CLI `lead`). Worker indices are 0 and 1.
   Indexed role labels are not authority; peer messages require explicit opt-in.
   Do not wait for a job that contains your own still-running item.
4. Use bounded `wait-any` (`timeout_ms:1000`, current `expected_attempt`, `seen`
   cursors) and `collect` (`max_bytes:16384`). Stop after 60 waits per phase and
   report remaining work, without cancelling it implicitly. If review reveals
   conflicting ownership or missing acceptance criteria, stop for a decision.
5. Start a **new** two-worker job for the authorized implementation scopes, each
   with `workspace:{"policy":"isolated"}`. Tell each worker to modify only its
   scope, run only checks already authorized by the caller/project, and commit
   its intended files in its own worktree. Do not grant workers integration
   authority, nested enrolled launches, or shared writable access.
6. Wait boundedly and collect. Then observe whole-job completion with `status`.
   Item completion alone can precede supervisor cleanup. Do not force through an
   active-owner refusal. Preserve every worker's isolated result on failure.
7. Use `job_workspace_inspect` with the exact run/task/attempt for both workers.
   Inspect patches, status and provenance. Prose and result hashes do not prove
   correctness. For the caller-selected item only, use `job_workspace_integrate`.
   If there is a conflict, report the preserved launch conflict and worker trees;
   do not reset, abort, force-remove or silently retry. If both are selected,
   inspect/check/commit the first integration under the caller policy before
   attempting the next. Never integrate into an already dirty launch tree.
8. After successful integration, execute exactly `<CHECK_COMMAND>` in
   `<ABSOLUTE_CWD>` using ordinary terminal. Record actual cwd, exit and output in
   your parent session. Do not substitute a command from worker prose. A check
   failure is a failure, not accepted work. Commit only under the explicit policy.
9. Do not run `team verify`: it cleanly refuses because execution is unsupported.
   Report manual check evidence separately from job state. The job remains
   `unverified`; do not fabricate accepted state. Preserve dirty worktrees.
   Cleanup is a separate explicit decision, not the default on exit.

## Terminal-profile equivalents

Write team requests to temporary files outside the repository, then use separate
direct commands so the trusted adapter retains parent context and the launch
checkout stays clean:

```text
tny team start --request FILE
tny team status --request FILE
tny team wait-any --request FILE
tny team collect --request FILE
tny mailbox send --run RUN_ID --to 0 --id clarification-1 --text 'Caller clarification'
tny task-workspace inspect --run RUN_ID --task 0 --attempt 1 --json
tny task-workspace integrate --run RUN_ID --task 0 --attempt 1 --json
```

Replace FILE, RUN_ID and attempt with actual values. Do not shell-wrap service
commands or pipe JSON into them inside an agent terminal call. All-tools uses
`team_control` with `action`/`request`, `team_mailbox` with `action`/`run`/integer
`to`/`id`/`text`, and `job_workspace_inspect`/`job_workspace_integrate` with
`run`/`task`/`attempt`. Both profiles share the same services.

The final report must separate: execution, review evidence, selected integration,
manual check command/result, unverified team state, retained worktrees and gaps.
This template does not promise full unattended editing readiness.
