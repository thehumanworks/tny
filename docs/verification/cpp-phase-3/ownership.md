# Phase 3 ownership inventory

Integrated source: `feat/cpp-ownership-137-139` (ADR 0118). Implementation
details and proof are recorded in evidence.md and the series evidence. All raw fd arguments to host seams are synchronous borrows unless
explicitly called consuming. OS-specific launch/staging/admission stays in C.

| Resource | Acquisition / transfer | Release and failure ordering |
| --- | --- | --- |
| Runner session writer | session_lock_acquire before reload, save, listener; existing caller lock borrowed | Spawn-acquired parent copy closes on every return; never LOCK_UN (child shares description). Child retains through engine/MCP quiescence, final save, log drain, socket unlink, then releases before bye. |
| Runner listener | unix_listen after writer; inherited by fork or mapped to fd4 in restart | Parent closes only its copy; failed fork unlinks while writer held. Child closes before socket removal. Restart borrows continuously, never releases between checkpoint and RUN. |
| Accepted clients | accept into one empty runner slot | Slot drop closes once; owner restart maps fd6, other clients reconnect; buffers are separate C storage. |
| Runner stderr tee | pipe, dup2 writer to fd2; reader transferred to runner | Both temporary ends closed on dup failure; fd2 process lifetime, reader closes on EOF or shutdown. |
| Frontend client | unix_connect then constructed client and role handshake | Allocation or handshake failure closes connection; close drains/frees queued messages and buffers, then descriptor. |
| Restart socketpair | socketpair then mapped spawn borrows child end | Parent closes child end after spawn; failure closes both, kills/reaps only spawned child, restores engine only if quiesced; payload secure_free. Successful RUN uses _exit, kernel closes parent copies without unlock or destructors. |
| Restart inherited fds 3/4/5/6 | fresh exec, validate packet and writer inode/lock; fd6 only if packet has owner | Error returns release admitted resources; successful child owns IPC/listener/writer/client. Secrets remain anonymous IPC, consumed checkpoint precedes pending effects. |
| Job state transaction | lock_open then bounded lock_try loop, load document | End frees document/directory and closes lock; commit remains explicit atomic persistence, never destructor work. |
| Reservation lock | lock_open, lock_try, inspect/update under lock | Close on every outcome; unknown owner/cleanup never reclaims. File record persists independently. |
| Submit/retry owner lock | lock_open/try before admission and job record writes | Borrowed by launch mapping; parent closes after launch/handshake; child retains. Failed admission only writes if no live supervisor. |
| Supervisor payload/ACK pipes | two pipes then mapped spawn borrows reader/writer/owner | Partial failures close acquired ends; successful spawn closes parent child-ends and transfers other ends to consuming handshake. |
| Worker inherited payload/ACK/owner | validate owner inode/description before state mutation | Payload closed after read, ACK after send; owner held through supervise/snapshot/reservations; all error returns must close owned fds. |
| Item prompt/output pipes and log | pipes then exclusive log open then spawn | Child ends closed after spawn; parent ends transferred to slot, EOF closes prompt; drain closes output/log after result observation. Launch failure closes all acquired ends. |
| Item POSIX pid | successful spawn returns unreaped direct child authority | Explicit cancel/stop/reap only; persisted pid never grants authority. Metadata, log drain, cleanup proof remain separate. |
| Item MSYS scope | scope_spawn owns admission reader, child wait authority, retained Job | Explicit ACK/GO/reap/cleanup/destroy; failed destroy must retain scope/unknown cleanup, never become success or free reservations. Lifetime Job stays in C process seam. |
| Process mapped staging | process.c duplicates each source collision-free, child mapping | Parent closes staged copies on every path; pre-exec child only C async-safe operations and _exit. |
| Private file writes | jobs_host.c temp open/write/fsync/close/rename | Fallible commit returns errno; temporary cleanup stays in C host seam. No owner destructor publishes status or retries persistence. |

The fork-only runner is an existing full child runtime, not the async-safe
pre-exec stub in process.c. Signal handlers only set sig_atomic_t flags.
macOS TLS policy and wasm unsupported policy remain unchanged.

## Delivered representation

- `src/util/resources.hpp`: `descriptor`, `lock_descriptor`, `pipe_pair`,
  `spawn_writer`, and `process_scope`. Integer views are explicitly borrowed;
  `release()` clears the old owner before handoff. All cleanup is noexcept.
- `src/core/runner.cpp`: `rn_state` and its `rn_client` array directly own their
  descriptors; the frontend client is constructed with `tny::make_owned` and
  destroyed with its matching deleter. Session lock ownership remains in its
  existing C slot; the scoped spawn guard owns that slot only when spawn
  acquired it. The fork child transfers the listener out of the parent stack
  owner. Successful restart deliberately calls `_exit`, leaving kernel close
  to release parent descriptions without unlocking the child.
- `src/core/jobs.cpp`: transactions own the state lock, directory and document;
  pipe pairs own partial setup; slots own prompt/output/log descriptors and
  native process scope. The POSIX PID belongs only to its slot's explicit
  cancel/reap protocol; it is -1 when a native scope owns the child. No persisted
  PID is used for signalling. Incoming supervisor descriptors are adopted before
  validation, so invalid requests also close their inherited resources.
- Native scope retirement is attempted before terminal result publication.
  Already-unknown cleanup does not retire a scope. Refused retirement sets
  unknown cleanup before persistence. Final explicit retirement failure returns
  EBUSY and prevents reservation release. The destructor transfers unresolved
  scopes into an intrusive C supervisor-lifetime list without allocating,
  waiting, signalling, publishing, or closing a retained Job handle.
- `process.c` mapped-source staging and pre-exec operations remain C, as do
  `jobs_host.c` file commits, advisory-lock syscalls and handshake consumption,
  and `session.c` writer operations. Their temporary descriptors remain owned
  by those documented consuming C seams, not by parallel C++ wrappers.
- `rn_client`/`rn_state`/`job_slot` use value initialization; `jobs_txn` has an
  explicit reset/destructor. Remaining memset/calloc in these modules apply
  only to C POD records or bytes, never nontrivial owners. Credentials keep
  secure_free and IPC-only transport. FILE-backed standard streams and fd2
  are process-lifetime borrows, not duplicate descriptor owners.

## Behavioral oracle mapping

| Boundary | Current check |
| --- | --- |
| Acquired and borrowed writer; parent-close; reuse; normal bye | `runner_repeated_lifecycle_preserves_descriptors_and_borrowed_writer`, 12 real runners |
| Writer at final save and unlink | `runner_ownership.cpp` invokes competing fresh-description lock probes inside both operations; early-release mutant fails |
| Held-lock refusal and protected listener/snapshot | runner contention/refresh tests; job repeated-invalid-admission fixture checks byte identity |
| Descriptor transfer/reuse | 200 real pipe/move/release/reuse loops; client transfer mutant fails |
| Partial job transaction, invalid worker, failed launch | 100 loops with real lock files and inherited pipes, no descriptor growth |
| Item spawn/pump/reap/drain | 40 real self-exec child cycles, exact log bytes, observed wait status and descriptor counts |
| Resource allocation | Actual client allocation failure after connect; live allocation balance and zero allocations during close |
| Pipe/listener/fork/save errors | Source-bound production fixture, actual acquired resources and per-case descriptor counts |
| Open/write/fsync/rename and staged dup1/dup2/spawn errors | Instrumented copies of unchanged C host seams; original file bytes and caller descriptors checked |
| Restart/checkpoint/consumption | Existing real PTY restart fixtures extended with listener inode and lock contention; provider/tool counts; consumed disk reader refuses second activation |
| SIGKILL vs destructor cleanup | New session fixture kills the lock holder, proves kernel lock release, reacquires and checks byte-identical snapshot |
| Cancellation authority | Metadata-PID mutant tested against a live unrelated sentinel; full interruption suite observes deadlines and writer release |
| Unknown cleanup | Unknown/complete record behavior, repeat hold/refusal fixture, native scope ownership source check; actual MSYS execution remains required |

The focused C++ fixture's item children execute its own stdin/log behavior.
It does not claim provider/job-schema end-to-end proof; the existing jobs and
artifact integration fixtures cover those and require coordinator teardown
capabilities. Exhaustive platform syscall failures, native Windows handle counts,
and the complete ps-dependent race suites remain unverified here.

Reproducible focused checks:

```sh
make test-runner-ownership test-runner-mutation
python3 tests/integration/test_jobs.py
python3 tests/integration/test_jobs_cleanup_hold.py
python3 tests/integration/test_job_artifacts.py
python3 tests/integration/test_background.py
```
