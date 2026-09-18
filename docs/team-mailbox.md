# Durable team mailbox service

Status: **service slice only** for issue #156, ADR [0139](adr/0139-durable-team-mailbox.md).
No CLI/tool registration or native delivery is included in this slice. Existing
synchronous `subagent.message` behavior is unchanged. The mailbox is collaboration
context, not a second execution queue, scheduler, daemon or provider loop.

## Security and authority: required integration

**The caller identity is trusted C input. Never construct it from an untrusted
JSON `sender` field, session ID, run ID or task index supplied by an agent.**
The API intentionally has no public JSON request decoder. A receipt's `sender`
is output metadata, not a way to authenticate a request.

`tny_mailbox_service.authorize` is mandatory. The service calls it under the
existing job's `state.lock`, with the current authoritative `job.json` bytes and
the trusted caller identity. The adapter must verify the privately handed child
run/task capability, or the lead's authority, against that record. It must also
enforce the caller's current permission/account ceilings. Return false to deny;
there is no session-possession fallback. The callback must be bounded and must
not reacquire locks, call a provider, or reenter the service.

The expected lead-owned capability design is a random member secret in the
private job launch payload, a verifier in the authoritative job record, and
child handoff through environment, **never argv, transcript or logs**. Capability
provisioning, verification and rotation are not implemented here. The focused
fixture's verifier schema is test-only, not a proposed public jobs schema.
A production allow-all callback would invalidate this contract.

The C identity binds:

- `run`: the existing durable job ID, exactly 32 lowercase hex characters;
- `job_attempt`: the current job attempt;
- `task`: stable job item index, or `TNY_MAILBOX_LEAD` (`-1`);
- `task_attempt`: the current item's attempt, or zero for the lead.

The service independently checks the complete job record's version, kind, ID,
items, indices, states, cancellation flags and attempts. It then checks both
endpoints and attempt fences under the same state lock used by jobs. A retry does
not silently adopt an earlier attempt's messages. Terminal or canceled recipients
reject **new** sends with `MAILBOX_TERMINAL`. Canceled/terminal senders also cannot
send new messages. Read, delivery and ack can finish for an existing current-attempt
message after cancellation. An exact duplicate send can retrieve its existing
receipt after cancellation; this does not enqueue anything new.

Lead-to-worker and worker-to-lead messages are permitted after authorization.
Worker-to-worker messages require the callback to set `peers_allowed` from an
explicit authoritative opt-in. The default is false. The service does not infer
opt-in from the payload or a session relationship.

Message text is **untrusted user context**. It cannot authorize checks, approval,
verification, integration, execution or changes to system instructions. Callers
must never evaluate it as commands. This is not a sandbox against other programs
with the same user's shell/file privileges. The job directory is a trusted,
canonical absolute path selected by the harness, not a request argument.

## C ABI and results

Header: `src/core/team_mailbox.h`. Every function returns `tny_mailbox_rc`.
`tny_team_mailbox_error(rc)` returns a stable static `MAILBOX_*` name.
There is no allocated result to free; callers own message structs and arrays.
Do not put large batches on a small stack. Output messages are valid only on OK;
`inbox` sets `count` to zero on failure.

| Function | Arguments after service and trusted caller | Behavior |
| --- | --- | --- |
| `tny_team_mailbox_send` | recipient `{task,task_attempt}`, ID, payload, byte length, output message | Persist queued record, then return receipt; exact duplicate returns original sequence/state |
| `tny_team_mailbox_inbox` | after-sequence, output array, capacity, byte limit, output count | Pure bounded ordered snapshot of caller's unacked messages, **including delivered** |
| `tny_team_mailbox_read` | ID, output message | Pure lookup for addressed caller and attempt, including acked records |
| `tny_team_mailbox_mark_delivered` | ID | Persist queued → delivered; repeated mark does not regress acked state |
| `tny_team_mailbox_ack` | ID | Persist delivered → acked; repeated ack succeeds; queued returns `MAILBOX_BAD_STATE` |

Read/inbox do not imply delivery or ack. An explicit public read adapter can use
read followed by mark-delivered when it has safely accepted the result; loss of
that response still replays until explicit ack. Only the recipient can read,
mark or ack. The sender can retrieve its receipt by repeating the same send.

Important errors: `MAILBOX_BUSY` (retry later; no acceptance), `MAILBOX_DENIED`
(authority, membership or peer policy), `MAILBOX_STALE` (attempt fence),
`MAILBOX_TERMINAL`, `MAILBOX_CONFLICT` (existing ID differs), `MAILBOX_FULL`,
`MAILBOX_HISTORY_FULL`, `MAILBOX_NOT_FOUND`, `MAILBOX_CORRUPT`, `MAILBOX_IO`,
`MAILBOX_INVALID`, `MAILBOX_UNSUPPORTED`. Error results never include payloads
or credentials. On uncertain I/O/client loss, retry the **same ID and content**.

## Persistence, limits and retention

Storage is `<job-dir>/mailbox.json`, version 1, written privately with
`tny_jobs_host_write_private`: file fsync, then atomic replacement. `state.lock`
uses the existing host nonblocking exclusive lock, tried once per call. No sleep,
spin, wakeup queue or backend work occurs here. Reads also take the lock. Jobs
must not call this service while already holding that lock.

The mailbox stores a run ID and an ordered `messages` array. Each entry contains
ID, sequence, job attempt, sender task/attempt, recipient task/attempt, state
(0 queued, 1 delivered, 2 acked), and payload. Array order and sequence must agree.
History IDs are unique; malformed history fails closed without overwrite.
Bounded confined reads reject symlinks/nonregular files and overlarge records.
Persistence is tested for process crash/reopen, not power-loss durability: the
existing host writer does not fsync the containing directory. No stronger
filesystem guarantee is claimed by this service.

| Bound | Policy |
| --- | --- |
| Payload | At most 16,384 UTF-8 bytes; embedded NUL and invalid UTF-8 rejected |
| ID | Caller-chosen 1–64 ASCII letters/digits/`.`/`_`/`-`; unique for entire run |
| Outstanding | At most 64 unacked records per recipient task, across attempts; delivered counts |
| Retained history | At most 256 total records per run, including acked messages |
| Inbox batch | 1–16 records and 1–65,536 payload bytes; earliest non-fitting message is not skipped |
| Record read | At most 25,427,968 bytes (worst-case JSON escape expansion plus metadata) |
| Job read | At most 4 MiB |

There is **no silent TTL, eviction or automatic compaction**. Ack frees an
outstanding slot, not a history slot. Retained full content provides exact
conflict detection and stable duplicate receipts for the entire run lifetime.
At 256 records, new sends return `MAILBOX_HISTORY_FULL`; exact duplicates still
work. Earlier-attempt unacked records remain retained and count toward the task's
limit, but cannot be delivered to a new attempt. This conservative backpressure
is intentional. A future explicit abandonment/archive policy needs its own
contract; do not delete history to bypass fences. Mailbox retention ends with
explicit removal of the enclosing job through existing job cleanup policy.
No mailbox API creates jobs, retries work or changes job execution authority.

## At-least-once delivery and lead-owned safe boundary

For native providers, the lead must call inbox at **`start_post`, before request
creation**, never during a tool or through backend reentry:

1. Revalidate the trusted capability and read a bounded batch (recovery starts
   at sequence zero; `after_sequence` is pagination, not acknowledgment).
2. Deduplicate by `(run, message ID)` against the **persisted transcript**.
3. Persist new messages as clearly labeled untrusted user collaboration context.
4. Mark each accepted or already-persisted ID delivered.
5. Use explicit ack to stop replay. Delivery is not acknowledgment.

A crash before transcript persistence causes a replay. A crash after transcript
persistence but before mark also causes replay; stable IDs suppress duplicate
context. A delivered message still replays until ack. A lost ack response is
safe to retry. None of this promises exactly-once reasoning or external effects.
Do not use an in-memory-only dedup set or advance a cursor past unpersisted text.
Long tools delay delivery until the next safe boundary; send does not interrupt
the tool. Each boundary must bound total batches as well as each individual call.

Host providers without safe injection must expose queued explicit read/next-turn
behavior, not interrupt/takeover as an approximation. SSH, embedded and wasm
execution mutation are unsupported: adapters set `native_local=false`, and the
service returns unsupported before file/lock side effects. The host execution
support check independently rejects wasm. This slice conservatively rejects all
mailbox operations in those modes; no wasm local-execution parity is claimed.

## Wiring and verification handoff

Lead integration still must:

- confirm source/build closure integration (`src/core/*.c` is already discovered
  by the Makefile; Nix includes `src/`), including unsupported wasm behavior;
- add the focused Python command to integration tests and Nix test dependencies
  (Python, native C compiler, vendored yyjson and the host seams listed in the test);
- implement private launch capability creation, verifier schema and locked
  authorization callback, permission ceilings and trusted directory resolution;
- register asynchronous send/inbox/read/ack CLI and tool operations without a
  public authorization-granting sender field, while preserving synchronous message;
- integrate native `start_post` transcript persistence/dedup, explicit ack, bounded
  polling and host-provider capability reporting;
- test public busy-tool delivery, clarification round trip, client loss/resume,
  permission/capability denial, platform rejection and real transcript crash dedup;
- run the integrated test/quality/leak, packaging, wasm, size and mutation gates.

Focused command: `python3 tests/integration/test_team_mailbox.py`. It compiles
and calls the actual C service and actual host seams. Tests cover durable order,
IDs, private files, restart, SIGKILL after accepted send/read/delivery/ack,
concurrent duplicate sends, corrupt records, symlinks, wrong membership/attempt,
capability denial, peer opt-in, cancellation, oversized payload, both capacity
limits and lock contention. The persisted-transcript fixture illustrates the
required consumer algorithm; **it is not evidence of integrated native delivery**.
Passing these tests does not complete #156.

Slice verification on macOS, 2026-09-18:

- `python3 tests/integration/test_team_mailbox.py`: 15 tests, exit 0.
- Same focused suite with `MAILBOX_TEST_CFLAGS='-fsanitize=address,undefined
  -fno-sanitize-recover=all'`, the host ASan runtime preloaded, and
  `ASAN_OPTIONS=detect_leaks=0`: 15 tests, exit 0. This is not a leak-gate claim.
- `make quality`: exit 0; GCC analyzer explicitly skipped on Darwin. The Makefile
  discovers the new C module automatically. Final `make format-check`: exit 0.
- Direct strict C11 compile, C++20 header compile and `clang-tidy` with the host
  SDK sysroot: exit 0. An initial standalone tidy invocation lacked that sysroot
  and failed to find `stdio.h`; corrected before the passing check.
- An initial background quality observation was interrupted without an exit
  status. It is not counted; the replacement full run recorded exit 0.

Full public integration, leak/mutation gates, Linux/Nix/wasm execution checks and
release size remain unverified in this isolated slice.
