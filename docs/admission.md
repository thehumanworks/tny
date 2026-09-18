# Shared launch admission

Status: helper implemented; **not yet wired into the jobs scheduler**. This is
part of #158, not completion of its two-real-batch acceptance test. Decision:
[ADR 0140](adr/0140-shared-admission.md).

## Scope and guarantees

`core/admission.h` exposes a C11 helper with a public C ABI. It grants permission
for an existing jobs owner to launch one existing item attempt. It creates no
worker, process, execution authority, provider loop, or new run identity.

Enrollment is opt-in. A scope is the tuple `(private root, public user label,
resolved provider/account alias)`. Both labels are 1–63 ASCII alphanumeric,
underscore or hyphen characters. Resolve provider aliases and the credential
source **before** enrollment. Use a public account alias, not a key, token,
credential hash, endpoint URL with credentials, or arbitrary environment value.
The grammar prevents paths and bearer-header syntax; it cannot determine whether
an alphanumeric string is a secret. The trusted resolver must enforce that rule.
Changing account identity requires a different scope; two aliases for one account
will not automatically share a limit. All supervisors must use the same canonical
root and resolved alias for the intended boundary.

Configuration is immutable after provisioning:

- `cap`: 1–128 simultaneously held launch permits.
- `queue_cap`: 1–128 waiting tickets.
- `claim_limit`: positive lifetime **launch-request claims** across this scope.
  Each fresh `(job ID, item index, attempt)` grant consumes one, before launch.
  This is NOT a count of provider HTTP requests. One admitted job attempt can
  make multiple model/tool requests. A failed launch still consumes its claim.
- Append-only identity history: at most 1,024 attempts, including cancellation
  tombstones. History exhaustion fails closed, separately from claim exhaustion.
  No automatic pruning, permit expiry, budget reset, or usage-based refund.

The existing local concurrency is an additional ceiling. A scope does not raise
it. Independent scopes do not constrain each other. There is no global provider,
account, token, cost, subscription, or exactly-once execution guarantee.
Usage is not stored or estimated here. Missing usage remains **unknown**, not
zero. SDK/durable attempt usage aggregation and provider-level request budgets
remain separate integration work. Explicit native child max_steps forwarding is
owned by the lead and is not duplicated here.

## API and state transitions

Call `tny_admission_apply(scope, attempt, operation, nested_enrolled,
cleanup_proven, &result)`. The result is valid only when the return value is zero.

| Operation/result | Meaning |
| --- | --- |
| `INIT` / `ready` | Explicitly provision or verify immutable configuration. Root must already exist. |
| `CLAIM` / `granted` | New grant committed; the existing jobs owner may launch once. |
| `CLAIM` / `owned` | Same-attempt replay. No charge, **no second launch authorization**. |
| `queued_capacity` | All shared slots are held. |
| `queued_fifo` | An older ticket must claim first; polling cannot overtake it. |
| `queue_full` | No ticket added. Retry only after space becomes available. |
| `exhausted` | Hard scope launch-claim limit reached; no new grant. Cancel remaining tickets. |
| `history_full` | No new identity can be recorded. Do not discard existing state to make room. |
| `INSPECT` | Reads one attempt and scope counts, without granting it. |
| `CANCEL` / `canceled` | Waiting ticket canceled, or pre-enqueue cancellation tombstone persisted. |
| `CANCEL` or `HOLD` / `cleanup_hold` | Granted capacity remains held pending owned-process cleanup proof. |
| `RELEASE` / `released` | Proof supplied by the jobs owner; active capacity freed, claim not refunded. |
| `not_found` | No matching identity; inspect/release/hold do not create one. |
| `busy` | Nonblocking state lock unavailable; no valid counts or ticket snapshot. Retry outside locks. |

Every successful non-busy result contains scope `claims`, `active`, `queued`, and
an identity's stable append-ordinal `ticket` (zero if no ticket was selected).
The reason string is available through `tny_admission_reason_name`.
`EINVAL` means invalid input or configuration mismatch; `EIO` includes corrupt
records; `ENOENT` means missing provisioned state; `EDEADLK` rejects nested
claims; `EPERM` rejects release without proof; `ENOTSUP` is unsupported execution.
Other host errno failures propagate. Never launch on an error.

Tickets use append order, not polling frequency. Only the oldest waiting ticket
can claim the next slot. The queue persists across supervisor restarts. A paused
head therefore blocks followers even when capacity is free: bounded fairness
means no overtaking, not a time guarantee. The jobs controller must cancel a
terminal or abandoned waiting attempt explicitly. Exhausted waiting tickets stay
recorded until canceled. Admission does not grant tickets in the background.

All mutations, including cancel-versus-grant, use the same nonblocking
`jobs_host` flock and a private atomic state-file replacement. Cancellation that
wins first leaves a tombstone; a later same-attempt claim cannot launch. If grant
wins first, cancellation creates a cleanup hold, not a free slot. At history
exhaustion, an unrecorded cancellation cannot create a tombstone; the jobs owner
must still persist job cancellation and prohibit launch.

## Ownership, recovery, and storage

The existing job owner lock remains the execution fence. Hold it across the
claim/launch handshake. The helper never interprets a PID. SIGSTOP, PID absence,
owner-lock availability, elapsed time, client disconnect, or supervisor death
alone never frees a permit. A crash before/after launch or during cleanup can
leave an `owned` permit with an uncertain process outcome. Recovery must expose
`cleanup_hold` with `HOLD`; it must not launch on a replayed `owned` response.
Even a fresh `granted` response is not an alternative to jobs ownership.

`cleanup_proven` is an assertion by the **trusted jobs owner**, not user input.
The helper cannot inspect process ownership or establish this proof. Only set it
when the existing jobs host/process scope proves cleanup of the owned process
and descendants, or when the owner can prove no launch occurred. Unknown cleanup
must call `HOLD`. A retained hold consumes capacity indefinitely; repairing it
requires the same ownership proof, not a force-release or PID-liveness shortcut.

Storage is `<root>/<label>/<provider_scope>/{state.lock,state.json}`. The root
and ancestors must be trusted private directories on one host with working
flock, atomic rename and fsync. Do not use network filesystems with weaker
semantics, replace lock files, copy live ledgers, or share this root across hosts.
The helper reuses `jobs_host` private atomic writes and locks. A small host seam
adds bounded no-follow regular-file reads and directory syncing after publication
and directory creation. State reads are bounded to 256 KiB. The file is 0600;
new directories are 0700. Parsed counters, unique identities, types and bounds
are checked before any transaction. Corruption or a missing state file never
implicitly resets claims. `INIT` is administrative provisioning, not recovery
from disappeared state; do not call it to replace lost history.

Persist-before-return includes file fsync, atomic rename and directory fsync.
A publication error can occur after rename; its outcome is uncertain. Do not
launch, refund, or assume the old state survived. Inspect under jobs ownership;
retain a hold if launch/cleanup is uncertain. Host durability still depends on
the filesystem honoring these operations. No claim of resilience to user state
deletion or storage corruption is made.

Acquire no job **state** lock while calling the helper, and acquire no admission
lock while doing jobs state transactions. The long-lived jobs **owner** lock is
not a state lock. Each helper call releases its state lock before returning.
Only bounded ledger read/validation/write/sync is done under it: no polling
wait, sleep, provider call, Git operation, process cleanup, or callback.

## Minimum safe nested policy

Reject every new enrolled submission from an enrolled worker or descendant,
including a different shared scope. Do this **before enqueue and before waiting
for child work**. Do not implement implicit permit borrowing or release a parent
permit while its process remains active. `CLAIM(..., nested_enrolled=true, ...)`
returns `EDEADLK` as a second guard.

The lead must derive this flag from trusted durable launch ancestry, not a
caller-supplied false value or editable environment alone. Guard the submission
entry point as well as the scheduler. Report an explicit `nested_admission`
error. Future handoff requires a separate design with full cleanup and authority
transfer; it is not part of this helper.

## Required jobs wiring (lead-owned)

1. Resolve and persist enrollment/configuration with existing job records and
   attempt identities; no second run record. Provision once at submission.
2. Keep local slot/dependency/permission checks. Enqueue only runnable items whose
   local launch slot is available; do not reserve shared permits for blocked work.
3. Outside job state locks, call `CLAIM` under existing jobs owner authority.
   Store ticket, reason and counters for inspect/JSON. Poll `busy`/queued with a
   bounded scheduler timer, never a state-lock wait.
4. Only `granted` proceeds to the existing launch path. Recheck cancel/deadline
   under existing job state synchronization before launching. If cancel won,
   release only with proof no launch occurred. Do not replay a fresh attempt for
   an existing `owned` response; enter recovery/hold instead.
5. On cancel, commit existing job cancellation and serialize the ticket cancel.
   On completion/failure, release only after existing scope cleanup proves the
   full owned process tree is gone. Update job status separately outside the
   admission state lock. Failure between these transactions must retain capacity.
6. Recover uncertain grants into explicit holds; cancel known terminal queued
   tickets. Preserve the same attempt key across lost acknowledgments/resume.
   A true retry must use the existing incremented attempt.
7. Enforce trusted nested ancestry at submission. Report effective local/shared
   limits, exhaustion and holds without exposing secrets.

### Two-real-batch acceptance still required

After scheduler wiring is supplied, submit two independent **real jobs batches**
against one deterministic provider fixture and one public scope with cap 2. Give
each local concurrency greater than 2. The fixture must record request entry/exit
and use barriers to ensure actual overlap; assert its observed peak is exactly 2
and never higher. Exercise FIFO cancellation, cancel/admit races, stopped and
killed supervisors, cleanup failure, lost acknowledgment, scope budget exhaustion,
same-attempt recovery and nested submission rejection through public jobs APIs.
Verify no provider request starts for a merely queued or replayed claim. Helper
contenders are not a substitute for these jobs fixtures.

## Capability and build integration

Native Linux/macOS: helper uses real OS locks/files/process capability checks.
macOS focused tests are recorded below; Linux execution is not yet verified here.
Native Windows/MSYS capability and directory fsync require platform validation.
wasm: clean `ENOTSUP` via jobs execution capability; no browser shared admission.
No remote coordination or cross-host SSH bound exists. Direct background turns,
SDK ephemeral workflows, host-owned loops and other launch paths are **not
enrolled**. They must not be described as globally constrained by this helper.

The root Makefile discovers the new core/util C sources through its wildcards.
The lead must register `python3 tests/integration/test_admission.py` in the normal
native test gate, and register the eventual real jobs fixture. Nix already includes
`src` and all `tests`, but the lead must confirm/update `nix/source.nix` and
`nix/tests.nix` alongside test registration and native compiler availability.
This branch intentionally does not edit Makefile, Nix or scheduler-owned files.

## Focused verification

Run `python3 tests/integration/test_admission.py`. It compiles the actual helper,
jobs_host, process seams and yyjson in a temporary directory with strict warnings.
Only rename and directory fsync have one-shot-style test controls; non-fault runs
call the real syscalls. The fixture tests real multiprocess cap 2, simultaneous
same-attempt claims (exactly one grant), persisted replay, FIFO, queue/history
bounds, claim exhaustion (including queued tickets), cancel races, paused/killed
owners, explicit holds, proof-required release, nested guard, config mismatch,
scope isolation, corrupt/missing/symlink state, private modes, lock contention,
and pre-/post-publication failures. A 60-second alarm bounds the process fixture.

Sanitizers: `ADMISSION_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
python3 tests/integration/test_admission.py`. Format/static checks use
clang-format, clang-tidy and Ruff. On macOS clang-tidy needs
`-isysroot "$(xcrun --show-sdk-path)"`. No queue latency improvement, hard cost
bound, or whole-product compatibility claim is inferred from these focused tests.
Full make test/quality/leaks, packaging, wasm, size, provider fixture batches and
queue benchmarking remain lead integration gates.

Recorded focused results on this helper slice (macOS arm64):

| Check | Result |
| --- | --- |
| Standalone native gate | Exit 0, including barrier-enforced peak 2 among 12 processes. |
| ASan/UBSan gate | Exit 0. Not a substitute for the product leak gate. |
| Project strict warning flags on new C files | Exit 0. |
| clang-format dry run, clang-tidy, Ruff check/format check | Exit 0; clang-tidy reports only suppressed non-user-header warnings. |
| Temporary-copy mutations: off-by-one cap, FIFO bypass, proof bypass, replay grant, nested guard bypass | 5/5 killed by runtime assertions; no compile-error kills. |

Initial check failures were corrected: the standalone link needed the existing
`process_scope.c`; macOS clang-tidy needed its SDK sysroot; sanitizer compilation
needed the same existing `-Wno-deprecated-declarations` baseline as the Makefile
for unchanged `util.c`; Ruff required import ordering. No product checks were
weakened. The installed `tny` has no `skill` command, so the advertised mutation
skill loader was unavailable; focused mutation probes used temporary source
copies and the standalone gate instead.
