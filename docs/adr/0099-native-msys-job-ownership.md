# Native MSYS2 job ownership scopes

Status: accepted. Date: 2026-09-12. Implements ADR0093's native jobs guarantee
and verification amendment A21; no platform scope is reduced.

A detached supervisor owns an anonymous, noninheritable lifetime Windows Job
containing itself and all item bootstraps. Its sole owning handle remains until
process teardown. Each item also belongs to a retained nested Job used for
individual cancellation. Neither breakaway flags nor persisted PID/Job names
grant cancellation authority. The submitting caller owns neither Job.

Retain existing POSIX spawn and staged descriptor semantics. A trusted item
self-admits before configuration or provider execution, closes its temporary Job
handle, sends exact private admission ACK and waits for explicit GO on stdin.
Admission is an event-loop phase outside state transactions; pending items count
against concurrency. Recheck attempt/cancel under a transaction before the
nonblocking GO transition. EOF, timeout, malformed control, failed assignment or
cancelled admission never release work. Errors after GO require started-work
cleanup. Strip private admission fields before normal execution and from inherited
and explicitly built environments.

The lifetime Job closes the pre-admission supervisor-loss gap even when a stopped
child holds an inner Job handle. Individual pre-GO cancellation may signal only
the directly spawned POSIX child while explicitly retained unreaped, with SIGCHLD
default and one consuming reaper. After GO, the retained inner Job is the sole
termination authority. Never signal enumerated or post-reap PIDs.

Keep observed root status, forced-stop reason, log drainage and scope cleanup
separate. A successful root exit does not discard surviving descendants; clean
the scope before terminal publication or slot/reservation release. Conversely,
force termination returning POSIX status0 cannot establish success. Verify
bounded Job accounting/membership observations, direct-child reaping and strict
absence of captured POSIX identities. Observational process handles never confer
signalling authority; uncertainty remains unknown.

Self-admission avoids replacing MSYS argv/environment/fd/exec bookkeeping with a
raw Win32 launcher. A lifetime Job plus per-item Job retains containment across
fork, exec, setsid and root exit without PID capture/reuse races. V1's per-item-only
last-handle guarantee was insufficient for stopped unadmitted children; it was
rejected before product implementation.

DESIGN-v2 and actual guest probes are retained in the task verification artifacts.
A21 requires real integration/admission-race, supervisor-loss, sentinel, normal-exit,
fd/environment, mutation and native x64 CI checks. Windows ARM guest probes are
supporting runtime evidence, not native x64 or completed-product proof.
