# Tenant record API migration

Migrate the whole repository from `RecordStore.lookup` to `RecordStore.resolve`.
The old method must be removed, including references in docs and clients. The
record schema and public client reader signatures stay stable.

`resolve(scope, identifier, *, as_of=None)` returns a `Resolution` with
`found`, `value`, `revision`, and `reason`. Reasons are `found`, `absent`,
`deleted`, or `expired`. A stored null value is found; it is never a fallback.
At a historical revision, choose the record with greatest revision not newer
than `as_of`. A tombstone masks older records. A record expires when the query
revision is greater than or equal to `expires_at`. A head query uses the
greatest revision anywhere in the store as the expiry clock. Negative
`as_of` is invalid. For absent keys, `revision` is null; for deleted or
expired keys it is the selected record's revision.

Every client module under `ledgerapi/clients` has a reader with a legacy
lookup call. Update all 30 call sites. `current`, `snapshot`, `summarize`,
`changed`, `select`, and `describe` expose them to the rest of the codebase.
Historical reads must pass `as_of`; the fallback applies only when `found`
is false. Keep the existing key format and public behavior for active records.

Run `python3 -m unittest discover -s tests -v` after setup.

This is a sequence of follow-up requests in one tenant-control-plane session.
Setup creates `fixtures/records.tsv` and three large JSONL resolution traces
under `evidence/`. Use them while investigating historical, null, expiry, and
tombstone behavior. Finish with `MIGRATION.md` at the workspace root.
