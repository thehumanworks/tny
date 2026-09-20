# ADR 0162: bounded review continuity on durable teams

- Status: accepted
- Date: 2026-09-20

## Context

Contribution contracts and typed messages improve identity and handoffs, but a
successful contribution does not prove acceptance. Dependency-delayed reviewers
can outlive producers, summaries need not describe the actual artifact, and
retained mailbox history is finite even after acknowledgment. Terminal profiles
also need recipes for tools they actually expose.

A three-worker swarm debated an acceptance ledger against a smaller review packet.
The counterexample was decisive: caller-supplied commands, exit codes and commit
references cannot become harness-observed verification merely by persisting them.
The selected design preserves observations and claims separately. It does not
pretend to close the automatic acceptance or revision loop.

## Decision

Extend the existing team-control adapter with root/operator-only `review` and
`review-read`. Record a bounded immutable snapshot of an integrity-matched
successful contribution after the whole run has terminal, settled cleanup. Fence
publication under the existing state lock and current attempt. Keep up to sixteen
numbered packets of at most 16 KiB per run, with at most 8 KiB of untrusted reviewer
claims per packet. Exact retries reconcile; conflicts, corruption and exhaustion
refuse. Do not open arbitrary claimed artifact paths, execute check commands,
change job verification, or create an acceptance authority.

Historical reads require current authorization but explicitly do not revalidate
source evidence. Recorded artifacts remain uninspected by this operation. Manual
inspection, integration and external checks remain separate decisions. Follow-up
work uses the existing parent-owned team/jobs path with explicit source identities
and findings in its prompt. Links supplied in claims are not authenticated DAG
edges. Completed purposeful members are not silently relaunched.

Add an authenticated `team_mailbox`/CLI `status` snapshot with retained history
usage and the caller's outstanding usage across attempts. Count publication
receipts individually. Report limits and remaining capacity without delivery,
acknowledgment, reservation, eviction or permission expansion. Acknowledgment frees
outstanding space only. Over-limit/corrupt state refuses instead of underflowing
remaining capacity. Status is occasional diagnosis, not a model polling loop.

Generate collaboration recipes according to the resolved tool profile. Terminal
profiles receive direct CLI commands; all-tools purposeful sessions prefer typed
named messages. Require actual artifact inspection and bounded explicit follow-up
requests; avoid ceremonial discussion and redundant coordinators. Preserve yolo
and shared writable defaults, explicit read-only ceilings, one scheduler, native
waits and the public C ABI.

Sanitize inherited runtime identities at integration-fixture setup, while allowing
explicit per-test overrides. Tests must be runnable from inside the harness and
must use localhost providers with synthetic credentials.

## Consequences

Review evidence survives session interruption without becoming accepted work.
A historical snapshot can be useful after source loss, but must not be read as a
current integrity check. Packet and mailbox capacities remain finite; there is no
silent history deletion. Reviewers still need the root's help to inspect isolated
artifacts or execute checks when their own authority is insufficient.

Native saved local Darwin/Linux use the existing services. Wasm, SSH and embedded
review operations refuse before effects. This change adds neither a scheduler nor
a check-process runner. Full automatic verification, authenticated follow-up
links, retained-history archival and empirical model-effectiveness comparisons
are intentionally not claimed by this ADR.

See [review workflow](../swarm-review.md) and
[verification evidence](../verification/swarm-review/evidence.md).
