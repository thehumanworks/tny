Implement #158 process-safe shared admission helper with real tests in isolated
feat/swarm-admission from 89bcd5918da0e225e1806813a206d007daafac0a. Read AGENTS,
issue158 and lead contract at /Users/tomas/.tny/worktrees/5fe4c489fba3f2e0/docs/verification/swarm-delivery/contract.md.
Own new src/core/admission.c/.h (C11) or util seam files as justified, focused
admission tests, docs/admission.md and ADR0140. Do not modify jobs.cpp/h,
Makefile/Nix, SDKs, subagent_plan, existing CLI/tool files or other ADR/ledger.
Core jobs worker owns scheduler currently; lead integrates helper there after
commit. Existing job id=run, item index=task, item/job attempt=attempt. Avoid
new execution authority: permits authorize existing jobs launches, not workers.
Opt-in shared scope identified by secret-free user label plus resolved provider
scope; immutable configured cap. Local concurrency remains additional ceiling.
Requirements: fair bounded durable queue, process-safe atomic transactions,
structured queued reasons, hard per-scope request-claim limit persisted before
launch, idempotent same-attempt claims (no double budget charge), release only
after caller proves owned process cleanup. Reuse jobs_host locks/private atomic
writes. Never reclaim a permit merely on PID absence, paused owners are live;
crashed uncertain launch/cleanup keeps capacity held, expose explicit hold.
Queue tickets may be canceled before grant; cancellation/admission races must
serialize. Lock ordering: no waits/provider/Git operations while state locks held.
Do not fabricate usage/cost hard guarantees. Unknown usage stays unknown; scope
contains no key/token data. Existing job records remain execution authority.
Nested enrolled submissions must fail clearly before deadlock or explicit permit
handoff; propose minimum safe policy and integration guard. Direct background/
SDK/host paths not enrolled must be documented, not implied globally bounded.
Pure helper API cannot prove actual batches cap2 alone: tests must exercise
multi-process claim/release, pause/death, cancel/fairness/exhaustion/idempotency,
and report exact scheduler wiring needed for two real fixture batches. Add real
integration once lead supplies jobs foundation; don't create a competing jobs fork.
Run focused tests and format/static checks. Commit coherent helper/tests/docs;
no push/PR/issue closure/merge or further delegation. Return SHAs, API, commands/
exits, accepted design, gaps and needed build/Nix/scheduler wiring. Lead already
implemented explicit native subagent max_steps forwarding; do not duplicate it.
