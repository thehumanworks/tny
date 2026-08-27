# Conformance adapter protocol v1

The orchestrator starts exactly one adapter command without a shell. It writes
one JSON request to the adapter's standard input and accepts exactly one JSON
value on standard output. Diagnostic text belongs on standard error.

Request:

```json
{
  "adapter_protocol_version": 1,
  "conformance_version": 1,
  "contract_sha256": "64 lowercase hex characters",
  "artifact": {"path": "/absolute/path", "sha256": "64 lowercase hex characters"},
  "secret_sentinel": "value which must never be returned"
}
```

The response contains protocol and product versions, adapter and SDK names,
ABI/library/platform/transport metadata, the independently verifiable artifact
hash and artifact kind (`shared`, `addon`, `wheel`, `package`, or another
contract-listed native form), semantic capabilities, the raw ABI capability snapshot, execution exit
codes, and one result for every scenario. Passing scenarios list the exact
assertion IDs from `v1.json`, reference at least one successful execution, and
include the normalized events that were actually observed.

The steer/resume scenario uses the contract's fixed `rejected_text` and exact
ordered terminal reasons (`interrupted`, then `done`). The validator requires
one matching `steer_rejected` event immediately before the interrupted terminal;
an adapter cannot substitute a fabricated marker or a private runtime fixture.

Before writing the final report, the orchestrator replaces volatile event
sequences/timestamps with stable ordinals and opaque provider/session/turn
values with presence booleans. Two successful executions against the same
artifact therefore produce byte-identical reports.

All capability keys and scenario IDs come from `v1.json`. An adapter returns
`unsupported` only when a required capability is false; `not_run` and `fail`
never release. The orchestrator independently hashes the artifact, checks
capability claims against the raw ABI snapshot, validates event order and
terminal state, and scans the complete response for forbidden fields and the
sentinel. Adding a language lane therefore needs only an adapter command; it
does not copy the transcript or report-validation logic.
