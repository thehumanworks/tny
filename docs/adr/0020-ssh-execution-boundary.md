# ADR 0020: `--ssh` is a process-level execution boundary

## Status

Accepted (2026-08-23).

## Context

Users want tny's tool calls (shell, file edits, MCP, skills) to run on a remote
machine while they type locally. The obvious design — an "ssh runtime" that
wraps each native tool call in `ssh host cmd` — is leaky: the native loop is
only one of four backends (Cursor bridge, Codex app-server, ACP hosts run their
own tool loops), paths in config/sessions/`--add-dir` would still be local, and
every new tool would have to remember to honour the wrapper.

## Decision

`--ssh user@host[:port]` delegates the **entire invocation** to OpenSSH before
tny loads any local config, session, workspace state, provider, MCP server, or
tool (`src/cli/ssh.c`, called first thing in `main()` after `--version` /
`--help`). The remote `tny` therefore owns every file, terminal, backend, MCP,
and session interaction; by construction all tool calls run remotely for every
provider.

- The rest of argv is re-emitted as a POSIX-single-quoted remote command, with
  `--ssh TARGET` removed; no local shell is involved.
- Targets accept `user@host`, `host:port`, `[ipv6]:port`; bare IPv6 with a
  port is rejected. Whitespace/control characters are rejected.
- Port goes through `-p`; host is passed after `--` so a target cannot be
  parsed as an ssh option.
- `/ssh TARGET` in the TUI drops prewarm + backend state, restores the
  terminal, and `exec`s the same path with a fresh remote TUI.
- Authentication and host-key policy stay with the user's OpenSSH config; tny
  never adds `StrictHostKeyChecking=no` or similar.
- `--ssh` reaching `cli_parse_globals` is an internal error (invariant guard).

## Consequences

- Remote host must have `tny` in its non-interactive SSH `PATH`.
- Startup invariant holds: no backend is spawned locally; `--ssh` costs one
  `execlp`.
- Coverage: `tests/integration/test_ssh.py` uses a fake `ssh` on `PATH` to
  assert argv shape, quoting, and error paths.
