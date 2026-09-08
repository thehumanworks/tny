# 0081 — Reliable session interruption under continuous output

Date: 2026-09-08
Status: accepted (amends 0053; relates to 0031, 0033, 0058)

## Context

A continuously readable OpenAI-compatible SSE stream kept `oa_dispatch`
inside its read loop, starving the frontend and runner control loops. The
runner client similarly drained until EAGAIN before parsing messages. It
applied a single-line limit to the entire unread burst. Ctrl-C could therefore
wait indefinitely, and event queues could overflow before the caller drained
them.

The TUI ignored a second Ctrl-C while cancelling. Its five-second fallback
sent another socket message, then displayed interruption without verifying
the runner had stopped. Foreground `ask` deliberately detached on a second
Ctrl-C. Ctrl-D could send cancellation immediately followed by socket EOF,
which discarded the unread commands. SIGHUP exited the TUI without stopping
the detached run.

## Decision

- Bound native HTTP dispatch to 8 KiB and runner socket reads to 64 KiB per
  loop iteration. Parse complete IPC lines after each read, retain split
  lines, enforce the limit per line, and process commands before EOF. The
  frontend gets another chance to process input under continuous output.
  Runner shutdown also gives queued final messages a bounded two-second
  drain before closing the socket: a large result must not be truncated
  merely because one nonblocking write filled the send buffer.
- First Ctrl-C requests cancellation. A second Ctrl-C while cancelling, or a
  five-second cancellation deadline, invokes an OS kill outside the runner's
  event loop. Palettes cannot intercept Ctrl-C during an active turn. Ctrl-D
  stops an active turn and exits even when the composer contains a draft.
- TUI quit/EOF and foreground SIGHUP/SIGTERM request shutdown and wait for
  it, escalating if needed. Signal handlers only record flags; cleanup runs
  outside the handler. Uncatchable client death (SIGKILL/crash) still detaches
  by ADR 0053. Explicit `ask -B` and observer detach retain their semantics.
- Share the force-stop implementation with `session stop --kill`. Validate
  the expected runner pid, terminate, wait for writer-lock release, acquire
  the writer lock for repair, recheck pid identity, and save interrupted/137.
  A failed kill, held lock, changed writer, or failed save is an error, never
  a successful interruption. Graceful cancellation retains partial output
  and exit 130; force kill retains whatever was already checkpointed.
- The foreground `ask` parent releases its inherited flock descriptor after
  a successful fork. Only the runner may retain it, so killing the runner
  actually releases the lock.
- On macOS/Linux, force cleanup enumerates descendants through the existing
  host OS seam (`util/process.c`), using libproc or `/proc`. It stops parents
  before enumerating children, then kills children before parents, including
  the separate process groups used by host providers and shell tools. The
  sweep is bounded at 4096 processes and reports incomplete enumeration as
  an error. Other native targets retain process-group termination. The
  current process and its group are never valid targets. Already detached
  jobs no longer descended from the runner and external attach targets are
  outside that ownership boundary.
- wasm keeps the bounded HTTP dispatch and in-process interrupt path. It has
  no runner; OS process termination reports unsupported. No new protocol,
  public ABI, thread, broker, or external executable dependency is added.

## Reproduction and verification

`tests/integration/test_interrupt.py` uses a real PTY and a local HTTP server.
The server first emits a synchronization token, then sends valid SSE events
without pacing or a terminal event. After at least 1 MiB of stream traffic,
the test sends Ctrl-C or Ctrl-D and checks saved status and connection close.
Graceful TUI cancellation may retain an idle runner; forced stop and exit
must release the writer lock. A separate SIGSTOP case prevents the runner
from servicing either signals or IPC; the second Ctrl-C must still kill it
and persist interrupted/137. Tests clean up only their own temporary sessions.

Against the pre-change release built from `1b61274`, both commands failed:

```sh
TNY_INTERRUPT_CASE=flood-ctrl-c python3 tests/integration/test_interrupt.py /path/to/tny-before
TNY_INTERRUPT_CASE=frozen-double-ctrl-c python3 tests/integration/test_interrupt.py /path/to/tny-before
```

Both ended with `AssertionError: session interruption did not complete within
3s`. The same cases pass after the fix. The full fixture also covers Responses,
in-process streaming, palettes, drafts, automatic escalation, Ctrl-D, hangup,
closing the actual PTY descriptors,
foreground CLI, and external `session stop --kill`.

Unit coverage checks split IPC boundaries, end-before-EOF, changed-pid refusal,
own-process refusal, terminal status repair, and force-killing a separate child
group that still owns the writer lock. Live OpenRouter billing/generation
behavior is outside these local protocol and process tests.

### Recorded local results (macOS arm64)

All commands used the repository toolchain through `mise exec -- env -u
TNY_TOOLS`:

| Check | Result |
| --- | --- |
| `make format` | Applied; final diff has no whitespace errors |
| `make -j8 quality` | Passed; 102 C translation units checked, plus strict warnings and Python/shell/workflow/JS lint. GCC analyzer is explicitly Linux-only |
| `make -j8 test` | Passed: 462 unit tests, 11,411 assertions, 44 integration groups |
| `python3 tests/integration/test_interrupt.py` | All 15 interruption scenarios passed, also included in `make test` |
| `make -j6 leaks` | Passed: zero leaks in the configured macOS gate |
| `python3 tests/mutation/mutate.py --focus session-interrupt --test bg_immediate_kill_refuses_changed_runner` | One valid guard mutant, caught by the unit test; no survivors; source restored |

Stripped release size: **901,104 bytes** on macOS arm64 and **968,504 bytes**
for the Linux aarch64-musl cross-build. The latter used the same Makefile with
`zig cc -target aarch64-linux-musl` (Zig 0.16.0), `UNAME_S=Linux`,
`UNAME_M=aarch64`, and `STATIC=1`. Linux execution, wasm execution, and MSYS2
execution were not performed locally.
