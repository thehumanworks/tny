Continue as sole owner of src/core/jobs.cpp/h, docs/jobs.md and test_jobs.py.
Your e351f40 is accepted as the foundation (not full issues). In YOUR isolated
control worktree first cherry-pick 4c7e0d3 and b491083 (workspace/admission helpers).
Those paths remain otherwise frozen; lead has the same commits integrated.
Read their headers/docs. Do not edit runtime/CLI/permissions/SDK or delegate.
Implement real scheduler enrollment for #157/#158 and private member-capability
provisioning for #156. Shared identity remains job ID/item index/attempt.

Submit schema (lead will mirror tool schema): opt-in DAG items accept
workspace:{policy:"isolated"|"shared_read_only"|"shared_writable",base?:COMMIT}.
DAG default shared_read_only; ordinary batch behavior unchanged. Isolated prepares
worktree OUTSIDE job state lock; persist preparation intent, then record returned
cwd/branch/base provenance and revalidate attempt/cancel before launch. On crash
never adopt foreign paths. Retain handle until cleanup proven; close never remove.
After owned child exits inspect provenance/diff under helper's bounds. No automatic
merge/cleanup or accepted state. Worker argv uses prepared cwd and verification
must open that cwd's session rather than launch checkout. Retry must not silently
rerun editing work in same dirty tree; explicit refusal is safer than destruction.
Lead owns explicit workspace inspect/integrate/cleanup CLI/tool and safety policy.
For shared_read_only set child env TNY_TEAM_READ_ONLY=1. Lead implements native
permission ceiling for this env including checkpoint retention and propagation.
Reject unsupported host read-only/workspace modes BEFORE launch/file changes if
host cannot enforce policy. SSH/wasm refusals remain before side effects.

Admission root request accepts admission:{label:ASCII_ALIAS,provider_scope:PUBLIC_ALIAS,
cap:1..16,queue_cap:1..128,claim_limit:positive}. Explicit immutable scope is under
ctx->tny_dir/admission. provider_scope is a PUBLIC user-chosen account alias, never
an API key; do not imply it automatically identifies all paths/accounts. Require
native provider for enrolled items. Persist enrollment and effective configuration
in job records/private payload; preserve retry. Use tny_admission_apply INIT once,
CLAIM outside state lock under supervisor ownership, launch only fresh GRANTED,
revalidate cancel/attempt before launch, CANCEL tickets on cancellation, RELEASE
only proven never-launched/cleaned children; HOLD unknown cleanup. Keep local
concurrency ceiling. Avoid keeping state lock during admission transaction.
Set TNY_ADMISSION_ENROLLED=1 in enrolled worker env; reject nested job submissions
with this trusted inherited marker before files/enqueue (helper EDEADLK too).
Lead adds native subagent refusal. Scope is opt-in top-level job launches, NOT a
global bound on arbitrary same-user shell processes; docs explicit.
Claim_limit is launch claims, NOT model HTTP requests/tokens; never overclaim.
Expose queued reason, effective limits and exhaustion. Available canonical usage
should be kept per attempt and summed without counting carried successes again;
unknown remains explicit. If actual token budget policy not implementable, report
gap, don't label claim count token/request budget. Propagate positive ctx.max_steps
in private payload and child argv with owned lifetime, preserve cap on retry.

Member mailbox capability for every DAG item launch (lead runtime already uses):
Generate random 32 bytes -> 64hex bearer BEFORE claim, persist only SHA256 hex in
item.mailbox_capability_sha256 under state.lock, never bearer in record/log/argv.
Child env TNY_TEAM_RUN=<jobid>, TNY_TEAM_TASK=<index>,
TNY_TEAM_ATTEMPT=<job attempt>, TNY_TEAM_CAPABILITY=<64hex bearer>.
Use private payload/worker environment; wipe bearer storage after spawn. Runtime
validates inherited identity against this verifier under job state lock. No
request field may supply bearer/verifier or impersonate parent_session. Optional
root peer_messages:boolean must be explicit and persisted (default false).
Lead has core/team_runtime.c with exported tny_team_capability_new, but do NOT
introduce dependency on it in your branch: use existing jobs hex/sha helpers.
Use existing env allowlist to strip parent team/admission fields unless assigned
to current item; never grant synchronous subagents the parent's membership.

Tests actual public job launch with two independent batches shared cap2 using
barriers; nested rejection; exhaustion; cancellation/owner-loss retains holds;
isolated workers edit same relative file without touching launch checkout;
readonly tool denial; private bearer not in record/status/argv; unknown usage.
Run jobs integration and focused ownership/quality. Commit new integration commit
without squashing foundation. No push/PR/merge/issue closure. Return exact SHAs,
commands/exits and any unmet scope. Lead owns full final integrated gates.
