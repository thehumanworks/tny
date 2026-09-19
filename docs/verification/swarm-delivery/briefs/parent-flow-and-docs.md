Finish integration coverage and user docs in a NEW isolated worktree based on
current lead HEAD. You own only NEW tests/integration/test_swarm_parent.py,
examples/swarm/*, docs/team-control.md, docs/team-mailbox.md,
docs/task-workspaces.md and docs/admission.md. No native/build/Nix/shared docs/ADR
or ledger edits. No delegation/push/PR/merge/issue closure.
Read actual lead working source at /Users/tomas/.tny/worktrees/5fe4c489fba3f2e0:
core/team_control.c,tools_team.c,team_runtime.c,tools_workspace.c,jobs.cpp; read
existing test_swarm_delivery.py. The lead has real all-tools/terminal mailbox,
client-loss, isolated-edit/conflict and usage tests passing. Your task adds the
CAPTURED TOP-LEVEL PARENT flow, not another helper-only fixture.
The service now permits a captured native parent to `team_control start` with
kind:ask,dag:true and >=2 worker items, no lead item. Root parent_session_id is
captured from that runtime, and mailbox parent is task -1. CLI operator-only start
still needs one explicit lead item plus >=2 workers. Indexed roles are descriptive;
peer messages require opt-in. This removes the deadlock of treating a lead item
inside its own still-running job as an integration operator.

Test actual public `tny ask -B` / native tools (and terminal profile if practical):
parent launches two workers through team_control, gets immediate job handle;
workers overlap; parent remains active and can send/receive clarification while
one worker's terminal tool is busy, with no replay. Parent uses bounded wait-any/
collect, then explicit task-workspace inspect/integrate after job completion,
then an explicit caller-configured check through ordinary terminal (no worker
prose authority). Prove recorded parent lineage, actual check result in parent
session, preserved isolated edits / optional conflict, and honest unverified job
state (team verify execution is unsupported). Do not fabricate accepted state.
Use deterministic localhost provider/barriers and fresh HOME/Git. No real keys.
Use existing fixture pieces as useful. Tests must run against actual shipped
public binary, not a standalone service driver or manufactured job records.

For checks, COPY the lead's latest build/tny into a private temp directory as
`tny`, then point TNY at it. This avoids concurrent lead rebuilds changing your
input. Record SHA256 and report source snapshot dependencies. The current binary
will be ready after /tmp/tny-swarm-checks/self-cancel-2.status appears; inspect
status/log, missing file is NOT success. Build is leader-owned; do not build/write
in lead checkout. The final integrated gate reruns your test against frozen HEAD.

Docs: replace helper-era "unwired/pending" statements with actual supported
surfaces/capabilities. CLI: team OP --request FILE|-; mailbox send/inbox/read/ack/
retire; task-workspace inspect/integrate/cleanup. Native all-tools and terminal
adapters share services. Wasm/SSH/embedded mutation unsupported. Host automatic
injection unsupported. `verify` cleanly refuses, no fake results. No mixed per-item
providers, no hard token/cost spending promise; scopes enroll only declared jobs,
nested enrolled jobs/subagents reject. Explain read-only default, explicit isolated
editing, recorded parent versus indexed members, no waiting for your own whole
job, explicit integration, dirty preservation, retired queues and unknown usage.
Include practical read-only fanout and review/implement/manual-verify templates,
with no unimplemented command examples. `tny ask --stdin < examples/swarm/lead.md`
is a one-command real parent launcher using provider/account already configured.

Run focused new fixture, Ruff/style and docs consistency. Commit coherent tests/
docs and return exact SHA/commands/exits, acceptance mapping, remaining gaps. Do
not overclaim all six issues or full unattended editing readiness.
