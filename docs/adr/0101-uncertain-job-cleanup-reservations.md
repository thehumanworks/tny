# 0101 — Preserve claims after observed job cleanup failure

Date: 2026-09-12
Requirements: R124 output isolation and ownership cleanup; contract A23.

## Decision

Persist an explicit boolean cleanup_hold with supervisor terminal state/cleanup:
it is true whenever cleanup is not complete. A true or malformed latch prevents
output-claim reuse, same-job retry and removal. Owner-loss projection preserves
an existing true latch. Release after complete cleanup remains unchanged.

False/missing is not sufficient evidence for unknown cleanup. For compatibility
with existing A11 projections, only the exact state=interrupted, cleanup=unknown,
error_code=JOB_INTERRUPTED tuple can be reclaimed after actual owner freedom
and a held nonblocking state transaction. Other unknown cleanup is held,
including a supervisor's interrupted/IO result after an unobservable root exit.
Read/lock failure denies. Retry checks its current transaction before mutation;
rm checks before rename/removal. Reservation locks remain held throughout the
owner/state check and claim update; no lock-order wait is introduced.

## Alternatives and consequences

Merely leaving reservation-file bytes behind does not preserve exclusion: the
next contender formerly reclaimed them after the supervisor exited. Classifying
all interrupted/unknown results as owner loss confuses projection with an
observed wait/cleanup failure. Blocking every projected owner-loss record would
remove A11's existing abandoned-owner reclaim behavior. The latch distinguishes
these cases explicitly while the strict fallback preserves existing records.

An uncertain cleanup can require external diagnosis before its output is reused;
no automatic recovery or new PID-signalling authority is added. A failed cleanup
never becomes permission to overwrite the same output through retry or rm.

## Validation

Independent reviewer /root/jobs_process_review challenged the original
state-only distinction and approved this corrected policy before code changes.
Tests must show post-supervisor-exit contenders, same-job retry and rm all refuse
observed unknown cleanup with unchanged records/claims; malformed values deny,
complete cleanup releases, and existing A11 owner-loss reclaim still works.
Compiled critical faults, actual native Windows lifecycle/descendant tests,
nonblocking contention cases and fresh corrected-source review remain required.
