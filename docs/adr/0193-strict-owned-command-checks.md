# ADR 0193: Explicit Linux owned command checks

Status: accepted, 2026-10-19.

## Problem and decision

A successful shell exit is not proof that its work finished: `sleep 30 & exit 0`
can leave a live child. Ordinary terminal commands deliberately retain their
shell semantics. Add opt-in `owned_check: true` to foreground `terminal` calls
through the existing execution server, without another scheduler or job store.

The existing fresh-exec command guardian becomes a Linux child subreaper before
launch. It alone owns and reaps its children. Its state machine is:

1. Validate the request and establish orphan adoption before spawning.
2. Observe the root command, deadline, interruption and private caller lifeline.
3. On root exit or cancellation, drain exited children and kill live direct
   children, repeatedly including descendants newly adopted after parent death.
4. Complete only when `waitpid(-1)` reports `ECHILD`. Keep root exit status
   separate from subsequent cleanup. Zero requires observed root exit zero,
   complete drainage and no forced killing of residual live descendants.

Child enumeration uses `/proc/self/task/<guardian>/children`. Before signalling,
`waitid(WNOWAIT)` establishes direct-child ownership while retaining PID identity;
the single-threaded guardian performs no consuming wait between this check and
signal. It never signals persisted PIDs, process groups or unrelated siblings.
A nested subreaper or `setsid` does not escape adoption when its ancestor dies.

The private guardian returns 130 after drainage on cancellation/timeout/caller
loss. A caller-side deadline or output limit independently fails the terminal
check with 125, even if a successful guardian exit races that limit. Residual live work
on natural completion, setup failure or unknown cleanup returns 125. Cleanup is
bounded to five seconds; a timeout is failure, not proof that all processes died.
The caller requires a positively observed guardian reap before treating zero as
success. After its bounded cleanup wait it returns unknown rather than using a
legacy process-group sweep or blocking indefinitely. A delayed guardian retains
its cleanup obligation; this cannot promise prompt absence on an uninterruptible
OS process. This is a check execution primitive, not a team acceptance receipt.

## Scope and limits

This is cooperative command ownership, not a security boundary. A same-user
program can kill its guardian; an OS-uninterruptible process may not terminate
within the deadline. Such cases must never create trusted success. Work handed
to an existing external daemon is not a descendant and is outside the scope.
Linux also requires readable procfs child enumeration and permission to set
subreaper status; setup failure launches nothing and returns 125.
On macOS, Windows and wasm this mode reports unsupported before launching work.
Ordinary terminal commands and durable background tasks are unchanged. The new
mode cannot be combined with `background` or `task_id`.

## Evidence

A private subreaper fixture reproduced the existing production guardian exiting
zero with a live adopted `sleep` child; the fixture killed and reaped that child.
The compact production-process integration suite covers clean exit, background
leftovers, nested subreapers/new sessions, timeout, SIGTERM, caller lifeline loss,
strict descendant absence and an unrelated live sibling negative control. A
separate test exercises the actual `run_code` → terminal → guardian consumer.
A pidfd-targeted guardian-crash fixture checks the public caller never reports
trusted success, without leaving background work.
No live provider credentials or OS security changes are required.

Linux CI requires the procfs capability and runs the ownership matrix. Restricted
local hosts without it test the fail-closed public consumer and explicitly skip
the success matrix; that skip is not ownership evidence.

## OS references

- [Linux child subreaper contract](https://man7.org/linux/man-pages/man2/PR_SET_CHILD_SUBREAPER.2const.html)
- [Linux wait/waitid ownership and WNOWAIT](https://man7.org/linux/man-pages/man2/waitpid.2.html)
