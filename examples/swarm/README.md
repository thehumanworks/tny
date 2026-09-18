# Native parent/team examples

With a native provider/account already configured, run from this repository:

```sh
tny ask --stdin < examples/swarm/lead.md
```

This starts a real parent that launches two read-only workers through public
services. It does not require keys in the prompt. Add `-B --json` for a detached
parent and observe the returned ID with `tny session ID --wait --json`.

| File | Use |
| --- | --- |
| [lead.md](lead.md) | One-command captured-parent read-only fanout; no indexed lead item. |
| [read-only.json](read-only.json) | External operator request: `tny team start --request examples/swarm/read-only.json`. Includes one indexed lead plus two workers. |
| [review-implement.md](review-implement.md) | Customize caller-owned scopes/checks before authorizing isolated edits and explicit integration. |

Operator and captured-parent starts are intentionally different. An indexed lead
is not the parent and must not wait for its own job or act as its integration
operator. Parent mailbox `lead` means task -1, not the item labelled lead.

Read-only is the default. Isolated editing is explicit; worktrees are not OS
sandboxes. No worker prose authorizes checks. `team verify` execution is
unsupported, and a successful manual terminal check does not mark the job
accepted. Unknown usage is not zero; admission covers only declared enrolled jobs,
not hard token/cost spending. Native local all-tools and terminal adapters share
the services. Wasm/SSH/embedded mutation and host automatic injection are
unsupported. See [team control](../../docs/team-control.md),
[mailboxes](../../docs/team-mailbox.md), [workspaces](../../docs/task-workspaces.md)
and [admission](../../docs/admission.md).

## Deterministic integration check

The fixture uses a fresh HOME/Git repository, a localhost scripted provider and
real public `ask -B`, native tools, terminal adapters and durable sessions. No
real provider key is needed. Point TNY at a private copy of the binary named tny:

```sh
TNY=/absolute/frozen/tny python3 tests/integration/test_swarm_parent.py -v
```

It covers both tool profiles, two overlapping isolated workers, parent lineage,
clarification during a busy tool without replay, bounded wait-any/collect,
post-job inspection/integration, a caller-configured terminal check persisted in
the parent session, retained worker edits and an honest unverified job. It does
not replace client-loss, conflict, usage, admission or platform-refusal suites.
