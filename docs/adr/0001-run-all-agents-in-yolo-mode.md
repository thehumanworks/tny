# 0001 — All agents run in yolo mode by default

Date: 2026-08-20
Status: accepted

## Context

tny drives four kinds of agents. Only one of them (the native openai loop)
gives tny a real approval gate. The host providers run their own loops:

- **cursor**: the SDK bridge is headless. There is no Allow/Deny RPC at all;
  `respond_permission` was a stub and tny printed a warning every session
  explaining that it cannot approve tool calls.
- **codex** and **ACP agents**: they *do* forward approval requests, but the
  host owns the sandbox and the tools; tny's gate is advisory.

So "ask" mode was a promise tny could keep on one provider out of four, kept
half-heartedly on two more, and could not keep at all on cursor. The default
(`ask`) produced friction on the native loop and warning noise on cursor,
while the actual security boundary always was the host's own sandbox plus the
user's decision to point an agent at a workspace.

## Decision

**tny is a yolo harness. Every provider runs in yolo mode by default.**

- The built-in default permission mode is `yolo` (was `ask`).
- Host approval requests (codex, ACP) are auto-accepted silently in yolo —
  no "auto-approved" chatter per call.
- The cursor headless warning is deleted; not being able to gate a host's
  loop is the designed state, not a defect worth a warning.
- `ask` and `auto` remain as *explicit opt-ins* (`--permission-mode`,
  `TNY_PERMISSION_MODE`, `permission_mode` in settings.json) because the
  native loop and the `tny acp` server contract (fx mode parity, editor
  clients switching modes via `session/setMode`) still use them. Opting in
  restores the previous behavior exactly.
- Noninteractive `ask` in explicit ask-mode still fails closed (exit 2 /
  deny) — unchanged.

## Consequences

- A fresh tny runs every tool call, everywhere, without prompting. That is
  the point: consenting to run an agent in a workspace *is* the approval.
- Anyone who wants gating must ask for it explicitly and should know it is
  only enforceable on the native loop (and advisory on codex/ACP).
- The permission engine (`src/core/perm.c`), rules in settings.json, and the
  y/a/n UI stay: they are the opt-in path and the ACP server needs them.
- Docs updated: `docs/features/permissions.md`, `docs/cli.md` defaults,
  AGENTS.md invariants. Tests assert the yolo default *and* the opt-in gate.
