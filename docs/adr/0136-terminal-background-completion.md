# ADR 0136: Owned completion for background terminal commands

Date: 2026-09-18. Status: accepted. Fixes issue #161.

## Context and reproduction

The native `terminal` background path returned a PID and log without reaping
its child. A finished build was reproduced under a live harness: its atomic
check-status file contained `0`, but `ps` showed the returned PID in state
`Z` with the harness as parent. PID existence cannot establish liveness or
success. The new `terminal_background_collects_actual_exit` regression also
fails against the original `run_background` implementation: no collectable
result or opaque handle exists. This is lifecycle evidence, not a benchmark.

The existing runner owns agent turns. The durable job service owns ask/image
requests, scheduling, reservations and retry. Adding arbitrary shell commands
to either would widen their contracts. `util/jobs_host` already supplies
private atomic publication and live owner locks; reuse that OS seam without
adding a daemon, scheduler, global SIGCHLD handler, or general job API.

## Decision

Native background commands get a private `~/.tny/terminal/<opaque-id>/`
directory, an exclusive owner lock, output log and atomic status record before
launch success is reported. A short-lived double-fork waiter owns exactly one
command. The caller reaps its launch child. The detached waiter resets its
own signal policy, closes inherited runner/provider descriptors, executes the
existing sandbox argv with the same cwd and nested permission ceiling, waits
for its exact child, and publishes its real exit code or signal before exiting.
Only the command is sandboxed. There is no `waitpid(-1)` or global signal
policy change in the caller. The OS adopter reaps the detached waiter, as for
other detached processes (Linux containers must use a reaping init).

The command does not inherit the owner lock. An exclusive live lock proves
that a waiter owns a nonterminal record, without consulting a stored PID.
Inspectors probe with shared locks, so concurrent readers cannot impersonate
an exclusive owner and revive an abandoned record. A lost waiter,
missing/corrupt record or unrecoverable publication becomes `unknown`, never
success. The command may continue after waiter loss; there is no invented
exit status. A failed launch acknowledgment is `launch_unconfirmed`, because
a timeout or broken pipe cannot prove that the command did not start.

Caller/runner loss and turn cancellation do not cancel accepted detached
work. A later turn or session under the same tny directory can collect it.
The logs and records remain until the user removes them; this change adds no
retention scheduler. Completion means the command shell was reaped, not that
arbitrarily daemonized descendants have exited or closed the log.

### Tool contract

All tool profiles use the same `terminal` schema and structured JSON:

- Launch: `{"command":"make quality","background":true}`.
- Inspect: `{"task_id":"<returned-id>"}`.
- Wait: `{"task_id":"<returned-id>","wait_s":30}` (0–600 seconds).

Launch returns `task_id`, `log`, and a ready-to-use `collect` tool call. States
are `starting`, `running`, `completed`, `failed`, `signalled`, `launch_failed`
and `unknown`. `exit_code` is null unless a normal exit was collected; `signal`
is null unless a signal termination was collected. `error_code` describes a
launch/observation error, not the command exit status. `status_source` names
`waitpid` on native hosts. No PID is accepted or advertised as authority.

`observation: timed_out` or `cancelled` stops only this wait. Neither implies
success, command cancellation, or permission to signal a PID. The wait pumps
the runner control channel and consults cooperative cancellation. Task
cancellation is deliberately not added: it needs separate reuse-safe process
ownership, not a persisted PID. Inspect/wait retain the `terminal` permission
identity. Supplying `task_id` together with a command/background flag is an
error. Foreground terminal execution and its cancellation remain unchanged.

### SSH and wasm

SSH keeps its existing POSIX-sh/coreutils-only remote requirement and detached
work behavior. A remote `nohup` shell owns and waits for exactly its command,
then atomically publishes its shell wait status under `~/.tny-bg/<id>/`.
All profiles expose the same handles, JSON, bounded waits and log paths;
inspection and logs refer to the selected remote host, never local tasks.

Without a remote locking/process runtime, an absent result cannot prove a
live waiter. SSH therefore reports `unknown` until publication, and bounded
waits can continue observing that state. Owner/transport loss also leaves
`unknown`; launch transport failure is unconfirmed. `status_source: shell_wait`
explicitly distinguishes POSIX shell status from native waitpid: status 143,
for example, cannot distinguish an explicit exit 143 from SIGTERM. `signal`
stays null on SSH. This conservative limitation avoids a new Python/flock/tny
remote dependency and never treats silence or a missing process as success.
Observation cancellation is checked between bounded SSH inspections; no SSH
observation signals the detached remote command.

Wasm cannot own native children or owner locks. Background launch and inspect/
wait return a clean unsupported error before creating files or spawning.
Foreground behavior is unchanged. No new platform seam or C++ scope is added.

## Verification

Unit regressions cover immediate 0/nonzero exits, signals, no output, launch
failure, bounded wait and observation cancellation, concurrent tasks with
foreground/provider children, descriptor isolation, caller loss, waiter loss,
invalid identities and profile parity. The SSH fixture collects both profile
shapes through the existing mock transport. The new loopback integration runs
all three profiles with isolated and in-process turns, collects across caller/
runner teardown, and asserts that the completed command is no longer present.
Targeted mutations check exit classification, exact-child wait failure and
owner-loss projection. Like other fork-heavy suites, `terminal_task_suite` is
excluded from macOS `leaks --atExit`: the inherited analysis hook stops the
waiter before its handshake. ASan/UBSan still runs it on macOS; Linux Valgrind
runs the complete unit suite, including these tests. Required suite/quality/leak results are recorded in the
PR; platform checks that could not run are explicitly identified there.
