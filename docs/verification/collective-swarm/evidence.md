# Collective swarm implementation evidence

Implementation owner: current worktree only. Baseline research commit `1ca48b3`.
The existing contract and research were read before implementation. Current user
handoff overrides publication: no push/PR/merge; primary assistant publishes.

## Mode checkpoint

Implemented global/ask-local parsing, slash selection, persistent mode/cap,
checkpoint fields, collective policy, shared admission injection and isolated
subagent refusal. Existing task and system instructions remain composed.
Native release `make -j4` exited 0 on the initial mode working tree (2026-09-19).
A following reviewer correction allows enabling mode in an existing idle session;
this correction still needs behavioral checks. Cap changes once enabled require a
new session so an existing immutable admission scope cannot be widened.

Read `/tmp/tny-collective-0qjnf5vp/review.txt`: addressed idle enablement;
publication collision/atomicity and subscribe-before-snapshot tracked for the
messaging checkpoint. Full functional verification is pending. Overall INCOMPLETE.
