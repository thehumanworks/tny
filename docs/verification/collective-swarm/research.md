# Collective swarm coordination research

Researched 2026-09-19, before implementing this change. Primary sources only.
Baseline: `3751ef9`; this repository already has durable jobs, DAG dependencies,
shared admission, authenticated per-attempt mailboxes, managed workspaces and
native safe-boundary context delivery. Reuse them, not another scheduler.

## Examples and design implications

1. **Claude Code agent teams** — https://code.claude.com/docs/en/agent-teams
   Independent teammates can exchange messages directly and coordinate through
   shared tasks. Automatic incoming delivery avoids model-driven inbox polling.
   Useful for challenging hypotheses and cross-cutting review, not merely returning
   isolated worker answers to a lead. Teams increase token use; more agents is not
   automatically better. Adopt direct peer access and shared objectives, not its
   external CLI dependency or platform-specific pane management.
2. **AutoGen topics/subscriptions and group chat** —
   https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/core-concepts/topic-and-subscription.html
   and https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/design-patterns/group-chat.html
   Topics separate addressing from implementation; one publication can reach
   multiple participants. A shared discussion does not require one transcript
   copied to every agent on every model call. Use the existing run as an isolated
   collaboration channel; retain independent cursors and addressed private replies.
3. **Anthropic's C compiler agent-team experiment**, published 2026-02-05 —
   https://www.anthropic.com/engineering/building-c-compiler
   Shared task claims and independently checkable tests helped agents cooperate.
   A single bottleneck led agents to duplicate and overwrite work: simply increasing
   the worker count did not help. Preserve explicit work ownership, existing DAG
   state and worktree isolation; share counterexamples and evidence early. Do not
   copy its unbounded agent-generation loop.
4. **NATS JetStream consumer semantics** —
   https://docs.nats.io/learn/jetstream/pull-consumers
   Durable delivery state and acknowledgements distinguish transport acceptance
   from consumer completion. Long-lived demand can avoid repeated short polling.
   Borrow bounded waiting, replayable delivery, stable IDs and explicit ack; adding
   a broker dependency would duplicate this application's durable mailbox.
5. **Prompt caching** —
   https://platform.claude.com/docs/en/build-with-claude/prompt-caching
   Caching operates on matching prompt prefixes. Keep policy/tool definitions
   stable; append bounded new evidence/messages rather than rebuilding a changing
   board in the system prompt. Actual cache hits remain provider-dependent and
   must not be claimed from a local mock.
6. **Patterns and problems in multiagent systems**, published 2026-08-13 —
   https://www.anthropic.com/research/multiagent-systems
   Interaction among individually capable agents needs explicit coordination and
   common objectives. Peer output is evidence, not trusted user instructions or
   permission grants. Preserve authenticated membership and inherited ceilings.

## Selected approach

An opt-in collective policy over existing native teams, with bounded collaborators,
shared run/channel publication, direct peer messages and kernel-event-backed
bounded waits. Jobs remain authoritative for task/attempt/dependency/outcome state.
The lead facilitates proposal, challenge, execution and verification; participants
can communicate directly, not only report to it. Specialised workflows use
existing DAG tasks, not a second workflow database.

At-least-once means accepted messages survive uncertain responses and can be
retrieved again until acknowledgement. It does not mean exactly-once model
reasoning/effects or guaranteed progress from a failed worker. Native safe-boundary
insertion deduplicates durable message IDs; waits need explicit deadlines and
terminal/cancel/error outcomes. Subscribe before reading to close the lost-wakeup
window; event hints are never the source of truth.

No quality, speed, token-saving or provider-cache percentage is claimed without
measurement. Local fixtures can prove stable request prefixes, bounded delta
insertion, overlapping peers and absence of model polling, not real-world model
quality or cache billing.
