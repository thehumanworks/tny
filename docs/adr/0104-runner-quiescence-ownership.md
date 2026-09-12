# 0104 — Hold runner ownership through quiescence

Status: Accepted — 2026-09-12.

## Context

ADR0053 makes the detached runner the sole session writer. Hosted Nix x86 CI
at f968265 failed immediate resume after client loss with an unreachable runner.
Static review found once-mode finalization releases the writer before engine
shutdown, a final save and socket unlink. A subsequent owner can therefore lose
its saved state or socket. Serve errors also release the lifetime lock, and the
TUI startup liveness probe does not acquire ownership before binding.

## Decision

Acquire the session writer before listener binding and child storage writes.
The spawn function preserves an existing caller-owned descriptor; if it acquires
one itself, it closes only its own parent copy after fork or on failure. The
child inherits ownership. Once and serve runners retain that ownership through
turn errors and finalization, engine/MCP shutdown, final save and log flush, and
socket removal. Release immediately before bye, after the last session mutation.
A failed acquisition performs no listener, pid, log or session mutation.

Resume reloads the resolved session under its acquired writer lock and reconciles
the fresh task snapshot before provider work. It never releases and reacquires
ownership while refreshing. Preflight validation before an explicit steer is
retained. New unsaved sessions are not reloaded.

Terminal status and turn_end describe the turn; writer-lock freedom and bye
establish runner quiescence. Existing busy behavior for another writer remains.
Tests waiting to resume an orphaned turn observe actual writer freedom, without
an arbitrary delay. Parent-side close after a caller-owned fork remains required
so a dead child cannot leave its caller pinning the lock.

## Alternatives and verification

Removing the final save alone leaves socket removal and engine hooks unprotected.
A sleep before resume hides the race. Binding before acquisition permits a stale
liveness probe to remove another owner's listener. None provides ownership.

Independent static review is recorded in the task's runner-quiescence-design
review.md. Verify existing isolation/background/steer/task and runner wire tests,
plus held-lock startup refusal, resume snapshot preservation, serve error ownership
and bye ordering. This decision records no runtime reproduction. Native C11 and
public ABI remain unchanged; wasm continues its in-process path.
