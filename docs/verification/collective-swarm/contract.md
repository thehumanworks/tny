# Verification contract: collective swarm mode

Date: 2026-09-19. Base: `3751ef9`. Branch: `feat/collective-swarm-mode`.
Worktree: `/Users/tomas/projects/tny/.worktrees/collective-swarm`.
Status: implementation delivered; final verification and PR delivery are being reconciled.
Unchecked requirements are not completion. See evidence.md for exceptions.

## User goal

`/swarm [n]` and `--swarm` select a lead/orchestrator mode for **collective**
participation, not isolated parallel delegation. Optional n caps the collaborators
(excluding the initiating lead). Without n, the lead chooses within existing
runtime safety bounds. Peers communicate directly and within an organised shared
channel. Communication is asynchronous/event-driven, durable at least once,
bounded and token/cache-conscious. Research precedes code, changes use a new
worktree, commits are incremental, and delivery ends in a PR.

## Required invariants and evidence

- [x] CLI global and ask-local entry, optional count, equals form, strict count
      validation, no accidental prompt consumption, help and slash completion.
- [x] Persistent/recoverable mode with explicit native-context refusal where
      unsupported; idle TUI rebind and session/workspace transitions are tested.
      Existing task presets, user system instructions and permissions survive.
- [x] Hard collaborator limit at launch admission, not prompt advice. Multiple
      starts/retries must not bypass it. One collaborator is valid. Unset means
      no arbitrary default chosen on behalf of the lead. Nested launch paths do
      not bypass the cap, and ordinary non-swarm operation stays unchanged.
- [x] Shared objective, independent proposals/challenges, peer replies and
      evidence-driven convergence are available through real public CLI/tool
      paths. Existing durable job/task/attempt/DAG state tracks workflow steps.
- [x] Run-scoped channel publication is atomic and replayable per intended
      recipient. Direct messages remain available. Stable IDs, authenticated
      sender/membership, attempt fences, ordering, bounded capacity/backpressure
      and acknowledgement remain intact. No new broker or scheduler.
- [x] Incoming waits use OS events, not repeated model calls or periodic mailbox
      scans. Subscribe-before-snapshot, deadlines, cancellation, terminal peers,
      queued-before-subscribe, concurrent sends and loss/reopen are covered.
- [x] Busy tools are not interrupted/replayed; native delivery saves context before
      marking delivered. No implicit ack, exactly-once effect, false acceptance or
      silently lost publication. Failures remain visible and finite.
- [x] Stable common policy/tool prefix; bounded newly received context. No full
      board rewrite per request. Measure fixture request/delivery counts and
      report cache capability separately from actual provider cache hits.
- [ ] New and affected native tests pass, including public localhost-provider
      multi-peer fixtures. Full `make test`, `make quality`, `make leaks` and
      release size/dependency evidence recorded with revisions and exit statuses.
      Proportionate mutation/negative checks and an independent review actioned.
- [ ] wasm/platform support explicitly documented and verified where available;
      unsupported execution fails before effects. Public C ABI stays unchanged.
- [ ] Research, ADR and user docs reflect final behavior. Incremental commits,
      remote branch and PR exist. Final report separates passed, failed, skipped
      and unverified evidence; no secrets or unrelated work are included.

## Evidence policy

Use isolated temporary HOME/provider fixtures with synthetic credentials. No live
feature-inference billing is authorised by a general build request. Coding-agent
review/implementation is permitted for this task. Use terminal exit status, not
PID existence, as completion evidence. Keep other worktrees and the original main
checkout unchanged. Do not merge, release or deploy the PR.
