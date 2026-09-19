# ADR 0150: Agent-first harness and measured footprint

Date: 2026-09-19. Status: accepted from explicit user direction.

Supersedes the product mission of beating Vercel fx on binary size, and the
artifact-size ceilings in [ADR 0121](0121-maintainable-cpp-and-six-megabyte-ceiling.md)
(and through it the earlier platform caps in [ADR 0120](0120-measured-linux-aarch64-cpp-artifact-budget.md)
and the size-and-speed Must column). It does **not** supersede startup, TTFT,
parser/event throughput, leak, ABI, permission, sandbox, or input/payload
bounds. Historical measurements in older ADRs and `docs/verification/` remain
evidence; they are not current acceptance rules.

## Context

tny began as a size-and-speed bake-off against [fx](https://github.com/vercel-labs/fx).
Later policy (ADR 0121) kept a decimal 6 MB artifact guardrail while preferring
maintainable C++ ownership over further byte-shaving. Both framed success as a
numeric binary ceiling and a competitor comparison.

The product is a harness for agents, built by agents, focused on the agent.
User constraints and tasks are the goal. A fixed binary-size ceiling and a
goal to beat fx fight that: they reward omitting context, compressing code,
and treating a human marketing comparison as the mission.

## Decision

tny is an **agent-first harness**. Optimize for effective context, reliable
discovery and editing, explicit authority, reversible operations, and
observed completion. Keep the binary fast, portable and small through
measurement. There is **no** product binary-size ceiling and **no** goal to
beat fx or any other competitor on artifact size.

Concretely:

1. **Effective context, not minimal tokens.** The agent must receive the
   instructions, tools, files and results the current task needs. Omitting
   required context to save tokens is a failure. Documented runtime payload
   caps (tool results, images, MCP bodies) remain safety bounds, not a
   token-minimization contest.
2. **Reliable discovery and editing.** Agents find and change the workspace
   through documented tools. Exact edits fail closed with actionable errors.
   No hidden IDE state.
3. **Explicit authority and reversible operations.** Permission mode,
   sandbox, worktrees, isolation and undo are named, observable, and not
   implied. Defaults stay yolo; ask/auto remain explicit opt-ins.
4. **Observed completion and verification.** A turn is finished when the
   required checks ran or blockers are named. CLI exit codes, `--json`
   fields, and the docs/test/quality contract are the evidence. Private
   reasoning need not be dumped; the final report stays skim-readable.
5. **Task constraints preserved.** User flags, project `AGENTS.md`,
   permission mode, provider/model/effort, workspace and extra dirs survive
   the turn and show up in status/session records.
6. **Model-agnostic interfaces.** One normalized event set and one CLI/TUI
   over Cursor, Codex, ACP and OpenAI-compatible providers. Switching
   `--provider` does not rewrite the task.

Footprint policy:

- Measure stripped artifact size (`wc -c`) and runtime dependencies
  (`otool -L` / `ldd`) on release builds. Report them. Do not fail a product
  gate because a byte count crossed a fixed maximum.
- Prefer maintainability, reliability, portability and measured speed over
  byte minimization. Do not weaken error handling, ownership, cleanup or
  startup to shrink the binary.
- Optional host binaries (bridge, ACP agents) stay external and are not
  counted in tny's artifact.
- Historical fx bake-off numbers and former 1 MiB / 1.5 MiB / 1.8 MiB / 6 MB
  ceilings may be recorded as superseded policy, and old sizes as dated measurements. They are not a product goal
  or an unmeasured claim about current fx.

Speed policy is unchanged: `--help` / `--version` stay
microseconds-to-milliseconds; TUI first prompt stays under 10 ms without a
backend spawn; TTFT client overhead stays measured. Same-host before/after
numbers are still required for performance claims.

## Consequences

Active contracts (`docs/product.md`, `docs/size-and-speed.md`, README, public
site) describe the agent-first mission and drop numeric artifact ceilings.
Build, Nix and CI retain artifact-validation and size-reporting targets, but
remove byte-ceiling enforcement. Packaging tests check accurate reporting and
accept valid artifacts beyond the former limits. `docs/verification/` snapshots stay as written.

New work still vendors libraries as source, measures C++ runtime
dependencies, and records size when a change is expected to move it. Size
is an observed property, not a budget the code must contort to meet.
