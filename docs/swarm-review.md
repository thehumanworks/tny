# Durable swarm review and follow-up

A durable contribution is not an accepted change. Use review packets to retain
what the harness observed about a completed contribution alongside explicitly
unverified reviewer claims. Packets use the existing team run, authority and
attempt fences; they do not execute checks or add a scheduler.

## Record a review

The submitting parent or a local operator can record a packet after the entire
team is terminal, its cleanup is complete, and no cleanup hold remains. The
selected contribution must have a successful, integrity-matched result. Failed
contributions remain visible through team status/collect; a packet does not turn
them into successful work.

Save a request file (replace RUN, ITEM and ATTEMPT with collected identities):

```json
{
  "id": "RUN",
  "item": 0,
  "expected_attempt": 1,
  "review_id": 1,
  "reviewer_claims": {
    "disposition": "revise",
    "criteria": [
      {
        "criterion": "Every split boundary is handled",
        "finding": "The recorded checks do not cover truncated frames",
        "evidence": "Parent session tool result; record the exact reference"
      }
    ],
    "artifact": {"commit": "EXACT_REVIEWED_COMMIT", "base": "EXACT_BASE"},
    "checks": [
      {"command": "make test", "cwd": "/absolute/review/checkout", "exit_code": 0}
    ],
    "unresolved": ["Run the missing truncated-frame check before accepting"],
    "follow_up": {"source_run": "RUN", "source_item": 0, "source_attempt": 1}
  }
}
```

```sh
tny team review --request /absolute/review.json --json
```

The example `checks` are **claims supplied by the reviewer**, not evidence of a
command this operation ran. Do not copy the example's successful exit code unless
that result was actually observed. `disposition`, artifact paths, commit names,
criterion text and follow-up references are also untrusted claims. This operation
never opens caller-supplied artifact paths or executes a claimed command.

`review_id` is an integer 1..16, naming one immutable slot in this run. Claims must
be a JSON object with unique keys, no embedded NULs and at most 8192 serialized
bytes. The complete packet is bounded to 16384 bytes. Unknown request fields are
rejected. Use the same ID and content on uncertain completion; changed content
conflicts. Claims retain JSON object-key order; reordering claims can conflict even
when the key/value set is equivalent. There is no overwrite, eviction or automatic
rollover.

The packet retains the recorded contribution/workspace provenance and collected
result/log integrity, separately from `reviewer_claims`. It remains
`verification:"unverified"`, `claims_verification:"unverified"`, and
`artifact_validation:"not_performed"`. A recorded shared-workspace baseline is
not an observed contribution HEAD. The packet is a snapshot, not a claim that an
isolated artifact was inspected, integrated or accepted.

`review` has its own sensitive `team_review` permission. An inherited read-only
workspace cannot record a packet. It can read an authorized packet through the
separate `team_review_read` permission. An indexed participant (even one labelled
lead) cannot record or read these root-owned packets.

## Retrieve historical evidence

```json
{"id":"RUN","review_id":1}
```

```sh
tny team review-read --request /absolute/read-review.json --json
```

Read uses current run authorization and confined private storage, but does not
revalidate the original result or artifact. `evidence_scope:"recording_time_only"`
makes this distinction explicit. Before using a packet as evidence of the current
checkout, inspect that checkout and rerun the necessary checks. Exact write retries
recheck source evidence; a retry can therefore refuse after source corruption even
when a historical packet remains readable.

Both actions also work through `team_control` with `action:"review"` or
`action:"review-read"` and the same request object. They are native, saved, local
CLI operations. Wasm, SSH and embedded/library contexts refuse before effects;
no public C ABI or browser implementation is added.

## Review the artifact, then repair explicitly

1. Collect every participant's terminal outcome. Do not hide failed peers because
   another contribution succeeded.
2. Inspect the exact contribution artifact, not just its summary. For managed
   workspaces, use [workspace inspection](task-workspaces.md) after owned work has
   settled. Check commit/base, patch, status, and intended file scope.
3. Run necessary checks in the intended checkout with explicit authority. Retain
   command, cwd, exit status and output in the parent session. Read-only reviewers
   must request executable checks from the root; do not instruct them to bypass
   their workspace ceiling.
4. Record the review packet with the criterion, inspected artifact identity,
   evidence references and unresolved findings. The packet preserves claims; it
   does not independently establish their correctness.
5. When revision is needed, the parent creates explicit follow-up work through the
   existing team/jobs surface, subject to the same admission and permission limits.
   Include the source run/task/attempt/review ID, exact artifact identity, findings,
   and a bounded repair/recheck scope in that new contribution's prompt. Do not
   relaunch an immutable purposeful roster or assume a completed peer can reply.
6. Inspect the repaired artifact, recheck the unresolved criteria, and record the
   next review in an unused slot or the follow-up run. The root decides whether to
   integrate and accept. No link in reviewer claims is an executable DAG edge or
   an authenticated relationship between runs.

Prefer a flat team unless a nested coordinator owns distinct synthesis work.
Independent review should challenge consequential assumptions, not manufacture
agreement. Shared writable work remains the default; declare disjoint file
ownership or explicitly choose isolated writers when scopes overlap.

## Scope and remaining work

This is durable **review evidence**, not an acceptance ledger, automated revision
loop, artifact mounting service or long-lived agent memory. `team verify` remains
unsupported. Machine-observed external check records, authenticated cross-run
follow-up edges, mailbox archival and larger current-main effectiveness experiments
remain separate work. No model-quality, latency or token-efficiency improvement is
inferred from deterministic integration tests.

See [ADR 0162](adr/0162-durable-swarm-review-continuity.md) and the
[delivery evidence](verification/swarm-review/evidence.md).
