# Independent review and resolution

Reviewed 2026-09-20 using a read-only `gpt-5.6-sol` high-effort Codex session.
The review completed with exit 0 and made no changes. Earlier design-review
findings about prepared permissions, exact rosters, frozen oracles, dynamic
provenance and omission markers were already actioned in the feature branch.

The delivery review identified three material findings:

1. Prepared message approval bound only a stored topology digest, not actual
   ordered membership. The send now rehashes names, roles, groups and coordinator
   links under the mailbox lock. Sender/unrelated-name/group change regressions
   refuse with no mailbox side effect; the unrelated-name regression first
   reproduced an incorrectly accepted send, then passed with the fix.
2. Failed isolated workers' retained workspaces bypassed terminal provenance
   checks. Prepared failed/cancelled workspaces are now inspected just like
   successful ones, without pretending a failed model result is successful.
   A deterministic post-edit provider failure retains an isolated commit;
   unchanged resume works and modifying retained files makes resume refuse
   before provider I/O. Failed workspace preparation remains covered separately.
3. Shared-workspace handoffs presented baseline revision as predecessor HEAD and
   missing dirty state as false. Both unobserved fields are now null; initial
   revision is named baseline_revision. The regression makes a shared-writable
   predecessor commit and dirty files before its consumer starts, then verifies
   the evidence does not invent a HEAD/clean snapshot.

The review's suspected cause of the earlier v1 resume failure was not treated as
fact. Direct reproduction isolated a different cause: optional v2 metadata was
persisted as null on v1/undeclared actors. Removing that fabricated presence fixed
the regression without weakening validation; a v2 omitted-field roundtrip also
passes. The reviewer did not rerun tests; direct-host results are the evidence.

The first broad verification attempt was deliberately stopped as superseded
before applying these new findings. Its nonzero exit is retained, not described
as success. Final serial verification and live evaluation are recorded separately.
