# ADR 0197: Preserve subagent budgets and expose actionable failures

Status: accepted

## Goal

A child must inherit the parent's effective step budget, and a failed child must
be diagnosable without replaying work or leaking provider diagnostics.

## Decisions

- Always snapshot nonnegative `max_steps`, including zero (unlimited). Omitting
  zero let a child reload a repository `steps` cap that its parent had explicitly
  overridden, causing an unexpected exit 2 after only that many requests.
- Retain `SUBAGENT_CHILD_FAILED` as the stable failure code. Recognize a small
  allowlist of runtime-generated failure prefixes and emit only static guidance.
  Unknown failures remain unknown. Never copy raw errors, stderr, credentials,
  provider categories or endpoint text into the parent tool result.
- Both stored-state actions report a safe failure reason. Explicit `inspect`
  exposes partial model output for stopped sessions, labeled partial rather than
  success. Running sessions do not expose a previous turn's result as current.
- Recommend inspection and continuation of the same session rather than blind
  recreation. A failed turn may already have performed effects; no automatic
  retry is added.

## Evidence

The fixture gives the repository a two-step cap and explicitly removes the cap
for the parent. Before this change the child fails after two requests. The
regression requires all six requests and success on Chat and Responses wires.
Existing positive-cap checks remain. Process-seam tests cover static failure
classes and malicious diagnostic sentinels, and stored-session checks establish
that partial output is recoverable while raw provider error text is not relayed.

These fixes establish a specific failure class, not the cause of every reported
exit 2. A user's exact incident requires that child's error metadata. Native
process behavior is unchanged; wasm retains its existing unsupported-launch
response and can read stored state.
