# ADR 0047: Scriptable workflow DAGs

- Status: accepted
- Date: 2026-08-29

## Context

tny already provides a strong single-turn CLI and low-level Python/TypeScript
bindings, but callers have to rebuild dependency scheduling, bounded
parallelism, result plumbing, and failure propagation in every script. Shell
backgrounding alone is insufficient: it does not validate cycles, preserve
ordered fan-in, distinguish failed from blocked work, or reliably terminate a
whole run.

The orchestration layer must not duplicate provider protocols or move policy
into a new daemon. It must preserve the CLI's broad provider support and the
native SDKs' owner-thread constraints.

## Decision

Provide the same DAG contract at three levels:

1. A sourceable Bash 3.2+/Zsh 5+ library executes one independent
   `tny ask --stdin` process per active task. Tasks are ephemeral by default.
2. Python `Workflow` executes one independent `AsyncRuntime` and session per
   active native task.
3. TypeScript `Workflow` executes one independent `Runtime` and session per
   active native task.

A complete graph is validated before execution. Ready tasks run up to a
positive concurrency limit. A task runs only when every direct dependency has
status `success`. If a dependency fails or is blocked, the task becomes
`blocked` without being invoked; unrelated branches continue. Results are
reported in definition order, while readiness is independent of completion
order.

Direct dependency outputs are appended to the consumer prompt in declared
edge order under a common labeled envelope. An edge can be ordering-only.
Combined dependency output has a configurable byte limit, checked before the
consumer starts.

Task failure is data, not an automatic fail-fast exception: callers receive all
reachable independent results and may then raise/exit. Definition errors and
run cancellation remain immediate errors. Cancellation terminates active shell
workers or reaches active native sessions and awaits cleanup.

The shell scheduler is file-backed for inspectable stdout, stderr, exit codes,
and status. It uses no `eval`, accepts only constrained task names, refuses
unmarked non-empty state directories, and runs its signal traps in a subshell.
The installed location is `<prefix>/share/tny/tny-workflows.sh`.

SDK workflow representations and aggregate errors omit prompts, runtime
credentials, output, and underlying exception text. Native permission requests
without an explicit workflow handler are denied so a headless run cannot park
forever.

## Consequences

- Shell automation can fan out across all CLI providers and SSH targets without
  learning provider protocols.
- Native SDK parallelism uses multiple runtimes rather than violating one-
  owner-thread/session constraints.
- Failure isolation retains useful results from independent branches.
- Dependency output remains untrusted model text. Labeling and byte bounds
  reduce ambiguity/resource risk but cannot eliminate prompt injection.
- Parallel write tasks can conflict at the filesystem level; callers should
  assign independent worktrees/workspaces and merge explicitly.
- Runs deliberately have no implicit cache, retry, distributed persistence, or
  resume semantics. Those policies remain visible in caller code until a later
  decision defines them.
- The shell compatibility suite runs under both Bash and Zsh in CI and Nix;
  native SDK integration tests prove multiple workflow runtimes against the
  strict local provider fixture.
