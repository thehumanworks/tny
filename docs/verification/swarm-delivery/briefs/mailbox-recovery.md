Continue your mailbox branch. Independent reviewer de590ef173142905 identified:
(1) After retry, old-attempt unacked messages consume all 64 outstanding slots,
but neither old (stale) nor new (fenced) caller can ack them. Add explicit,
authorized durable retirement of old-attempt messages, retaining dedup tombstones.
(2) jobs_host_write_private fsyncs file then rename, not parent directory. Improve
that host seam to fsync parent before success acknowledgment; failure after rename
must report uncertainty/error, never success. Admission has an existing parent-sync
implementation to consult at b491083 src/util/admission_host.c. Process crash and
power-loss contract must stay honest; syscall-order fault tests required, no fake
powerloss proof. Own src/util/jobs_host.c/.h for this directly related fix, plus
mailbox helper/tests/docs/ADR0139 (still proposed/unpublished). No jobs.cpp/h,
Makefile/Nix, core runtime, CLI/tool schema or other worker files.

Proposed API: add tny_team_mailbox_retire(service, trusted_caller, recipient_task,
before_job_attempt, size_t *retired). Only authenticated lead (-1) can retire;
before_attempt must be <= current job attempt, and only records strictly older
than before_attempt. Require recipient membership; no arbitrary path/identity.
Mark retired records with a distinct retained state, never deliver them to a new
attempt; release outstanding quota only. Acked messages retain original receipts.
Explicit op, no automatic eviction. Wrong role/current-attempt retirement must
fail closed. Tests: full attempt1 inbox -> retry attempt2 -> full until explicit
lead retirement -> new message accepted; old IDs remain conflicting/idempotent
according to identity fences. Add role policy tests. Current indexed role:"lead"
remains descriptive; routing semantics will be settled centrally, don't widen it.

Record exact checks/exits. Commit coherent fixes; no push/PR/issue closure/merge
or further delegation. Final helper code must be actual tested behavior, not
adapter stubs. Lead wires retire CLI/native operations and context-safe delivery.
