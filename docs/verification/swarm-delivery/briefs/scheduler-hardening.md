Your b106c3c is integrated. Continue as sole jobs.cpp/h and test_jobs.py owner.
Cherry-pick 08a7108, then 1f52536 (mailbox/host sync), then f798bc6 (lead glue,
read-only/checkpoint ceilings, runtime mailbox) into YOUR control worktree. These
are all disjoint from your jobs changes; resolve only mechanical header context.
This lets your actual read-only test run: TNY_TEST_TEAM_READ_ONLY_ENFORCED=1.
Do not edit files from those commits outside jobs.cpp/h,test_jobs.py,docs/jobs.md.
Lead is concurrently wiring newer workspace and team adapters; no delegation.

Mandatory independent-review fixes (de590ef173142905):
1. jobs_cancel reads state then reacquires txn without expected attempt. Require
expected_attempt for DAG cancellation and compare in the SAME transaction before
writing flags. Preserve ordinary batch grammar/semantics. CLI --expected-attempt N
and typed JSON field expected_attempt; stale attempt returns explicit error with
no flags changed. Tests deterministic cancel/retry race, not sleeps. Team control
adapter passes expected_attempt. Existing tests must explicitly supply current
DAG attempt; do not weaken tests to bypass fence. Internal operator cleanup tests
may inspect status first, but assert stale behavior independently.
2. DAG retry currently checks provider name/model/effort but can switch endpoint
or account. Persist a secret-safe execution-scope fingerprint, before launch,
covering effective endpoint, account identity, credential source/account as needed,
auth routing and policy config/extra dirs. NEVER persist raw keys/tokens/URLs with
secrets. A one-way fingerprint is an integrity fence, not authorization. Where
account identity is unavailable, conservatively fence credential change (document
key rotation needs a new explicit run). Known ChatGPT account can survive token
refresh without silently switching accounts. Compare before reusing outputs or
spending; tests change endpoint/key/account/policy and prove zero new provider work.
3. Enrolled team members must not use legacy jobs control to cancel/retry/remove an
unrelated run. Capture parent session only from tny_jobs_run_context. If inherited
TNY_TEAM_RUN exists, validate private capability SHA256/current task+attempt against
that run under appropriate job state authority before sensitive operations; never
trust a request sender/session. Restrict worker control to own task, or refuse
unsupported member controls explicitly. Local CLI operator outside nested contexts
retains existing user authority. Add unrelated-run sentinel test through actual
CLI/tool routes. Do not rely on possession of a job/session ID alone.

Remaining #158 budget work, AFTER above correctness fixes:
- Add opt-in run soft token budget using actual observed usage at safe scheduler
boundaries. Explicit policy stop/cancel pending admissions when exhausted; missing
usage triggers explicit unknown policy (default stop when a token budget is set),
not zero. Already admitted work may overshoot: state that clearly. Persist across
retry/accounting, don't double-count carried artifacts. Test deterministic usage.
- Existing claim_limit is a hard launch-request cap. Keep that label, not HTTP-call
or money limit. If adding a model-call allowance, reserve each admitted child's
positive max_steps upfront and never admit more reserved steps than available;
HTTP retries/host loops are NOT covered and must be stated. Do not fabricate a hard
billing guarantee. Prefer small explicit native-only policy over complex framework.
- Optional absolute run admission deadline can reject new launches after expiry;
retain it on retry. Active cancellation must use existing owned cleanup paths.
Nested enrolled launches already reject; document that as a hard depth-one policy,
not arbitrary shared limits for unenrolled shell/SDK/host work.

Run full jobs suite, read-only enforcement, core job/ownership/static checks and
new fault tests. Commit coherent fixes and report exact results and any remaining
gaps. No PR/push/merge/closure; lead integrates final gates. Do not modify other
workers' files, add placeholder stubs or claim full swarm acceptance here.
