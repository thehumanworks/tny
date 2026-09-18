Implement durable mailbox service for #156 in isolated feat/swarm-mailbox.
Base 89bcd5918da0e225e1806813a206d007daafac0a. Read AGENTS, issue156, required docs,
and lead contract at /Users/tomas/.tny/worktrees/5fe4c489fba3f2e0/docs/verification/swarm-delivery/contract.md.
Own NEW src/core/team_mailbox.c/.h, tests/integration/test_team_mailbox.py or
focused test fixture, docs/team-mailbox.md and ADR0139. No jobs.cpp/h, native
loop/tool/CLI registration, Makefile/Nix, or other workers' files. Lead integrates
these after your commit. No nested agents, push/PR/merge/issue closure.
Shared identities: existing durable job ID (32hex) = run; item index = task;
job/item attempt fences attempt. Existing job record is execution authority.
Build real C service over existing util/jobs_host private atomic writes and
bounded nonblocking locks, not another execution queue/daemon. C ABI operations
send/inbox/read/ack. Persist before send acknowledgment; stable caller-chosen
message ID idempotency and conflicting-content rejection; ordered sequence;
separate queued/delivered/acked; explicit at-least-once replay/dedup; <=16KiB
payload and <=64 outstanding per-recipient, bounded retention/backpressure.
Messages are untrusted user context, no executable checks/system instructions.
Validate complete job/run/task membership and attempt fences; recipients terminal
or canceled reject clearly. Peer messages require explicit opt-in; lead<->worker
allowed. Do not treat arbitrary session possession as membership. Service caller
identity must be an explicit TRUSTED C input separate from untrusted JSON args:
lead will derive that identity using privately handed child run/task capability
and authoritative job record. Expose this requirement prominently; never offer a
public `sender` request field that grants authorization. Agent running task
capability design currently expected: job payload carries random member secret;
job record stores verifier; child env carries secret, never argv/logs; no implicit
claim that same-user shell privileges are sandboxed. If your safe design can
avoid this seam propose it, don't fake authentication.
Native backend delivery will happen at start_post before request creation, never
reenter a backend or interrupt tool. Provide helpers for bounded read/mark-delivered
and explicit ack, with stable IDs to dedup against persisted transcript. Lead owns
that integration. Host providers only queued explicit read/next-turn if no safe
injection. SSH/embedded/wasm execution mutation unsupported before side effects.
Test actual durable records with order, IDs, crash/reopen, full/oversize, wrong
member/attempt, canceled recipients, duplicate/conflicting sends and lock contention.
Use existing test mechanisms; return needed wiring. Run focused tests and quality,
commit real code/docs/tests and report commands/exits, exact API and unfulfilled
public integration criteria. Do not call helper-only tests completed #156.
