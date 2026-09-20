# Research and design inputs

Reviewed 2026-09-20 alongside the baseline implementation and ADR 0156.

- AutoGen Swarm: https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/swarm.html
  Uses capability-based handoffs over shared conversation. Useful local decisions,
  but turn-taking/shared transcripts are not a model for concurrent durable tny peers.
- AutoGen core group chat: https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/design-patterns/group-chat.html
  Typed event/topic communication offers an example of separating runtime delivery
  from speaker policy. Reuse tny's existing authenticated durable channel instead
  of introducing this dependency or a second event loop.
- CrewAI crews: https://docs.crewai.com/en/concepts/crews
  Explicit agent membership and manager-coordinated processes inform reusable
  definitions. Definition validation and runtime authority must remain separate.
- Codex subagents: https://developers.openai.com/codex/multi-agent/
  Task-specific agent configuration and orchestration provide the comparison
  context. Benchmark installed Codex, recording its exact version and settings;
  do not use vendor examples as measured evidence for tny.

Baseline findings: existing swarm mode has a stable collective policy, shared
admission, durable fanout and native mailbox directory watches. It forbids all
nested collaborators and has no swarm definition format. Audit completion waits,
context fanout, bounded-history pressure and queued-peer waits before extending.

Proposed direction: validate a bounded definition tree and compile its members
into the existing job runtime, retaining explicit coordinators/group provenance.
Avoid independent recursive schedulers and hold global capacity authoritative.
Measure correctness and overhead, not only whether multiple agents were launched.
