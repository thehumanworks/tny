# Collective swarm implementation evidence

Implementation owner: current worktree only. Baseline research commit `1ca48b3`.
The existing contract and research were read before implementation. Current user
handoff overrides publication: no push/PR/merge; primary assistant publishes.

## Mode checkpoint

Implemented global/ask-local parsing, slash selection, persistent mode/cap,
checkpoint fields, collective policy, shared admission injection and isolated
subagent refusal. Existing task and system instructions remain composed.
Native release `make -j4` exited 0 on the initial mode working tree (2026-09-19).
A following reviewer correction allows enabling mode in an existing idle session;
this correction still needs behavioral checks. Cap changes once enabled require a
new session so an existing immutable admission scope cannot be widened.

Read `/tmp/tny-collective-0qjnf5vp/review.txt`: addressed idle enablement;
publication collision/atomicity and subscribe-before-snapshot tracked for the
messaging checkpoint. Full functional verification is pending. Overall INCOMPLETE.

## Messaging and public behavior checkpoint

2026-09-19, implementation after c20d17d, checkpoint content recorded by the next
commit. All commands ran from the assigned worktree, native Darwin arm64, pinned
mise tools, synthetic localhost credentials only.

- `make -j4`: PASS exit 0 (build-formatted.log). Native Release 1,128,608 bytes
  before final policy wording; final footprint to be measured again.
- `make test-unit -j4`: PASS exit 0 (unit1.log), including strict swarm count and
  failed task/session reconcile cap rollback regression.
- `python3 tests/integration/test_team_mailbox.py`: PASS exit 0, 31 cases
  (mailbox-final.log): original 25 plus publication snapshot/retry/ack,
  transaction-wide backpressure/collisions, queued/timeout/cancel/terminal/stale,
  quiet single snapshot, notification race and replacement.
- `python3 tests/integration/test_collective_swarm.py`: PASS exit 0, 11 cases,
  7.077s (collective3.log): both public tool profiles exchange a proposal,
  counterexample and reply before convergence; n=1; launch bypass refusals;
  global/ask-local mode and count syntax; task/system preservation; save/resume;
  stable system text; native PTY idle enable/rebind plus /new cap change;
  terminal publication fails nonzero. Two-peer flow uses exactly 4 lead, 5 proposer
  and 6 challenger requests, not model inbox polling.
- `python3 tests/integration/test_collective_cap.py`: PASS exit 0, 1 case,
  1.765s (cap2.log): two separately started real runs share cap1; second queues
  until first exits. Seven parent calls, one per worker. Also public CLI empty,
  deadline and SIGINT cancellation (exit130) without extra model calls.

Earlier failures retained outside repo in the handoff directory: mailbox-new.log
(quiet lock-creation hint and lock-contention race), collective1.log (reply BUSY),
collective2.log (PTY assertion expected an untruncated mock echo). Fixes: bounded
transaction-lock retry, initial quiet fixture normalization and correct PTY oracle.
These failed runs are not represented as passes.

Independent read-only Codex review reported failed-resume cap rollback, /new cap
change refusal and omitted timeout schema. All corrected; rollback has a unit
regression, /new has PTY coverage, schema regression is being added. Coordinator
also reproduced idle writer handoff race, fixed by graceful end/reap followed by
locked reload before binding/saving mode. Additional reviewed fixes: bounded
watch draining; opt-in-only collaborator policy; safe adoption of old owned work;
legacy retry refusal; permission detail includes action; only wait treats terminal
as observational success; publication receipts omit N copies of the payload.

Current delivery: atomic publish over existing records, per-recipient replay and
ack; kernel-notified bounded wait with separate outcomes; runtime shared admission
across launches; durable opt-in collective policy. Full gates, Linux/wasm execution,
mutations and remaining focused regressions are still pending. No cache-hit or
real-model convergence/quality claim. Primary assistant will own full `make test`
and frozen wasm checks per coordination file; this process owns quality/leaks and
focused followups. Overall verification gate remains INCOMPLETE.
