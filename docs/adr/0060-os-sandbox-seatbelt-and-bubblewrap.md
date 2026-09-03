# 0060 — OS sandbox: Seatbelt and bubblewrap for the terminal child

Date: 2026-09-02
Status: accepted

## Context

The shell-first native-loop direction in ADR 0057 keeps an OS sandbox as one
of the harness's three security seams. Permissions answer whether a command
may start; they do not constrain what an allowed shell and its descendants can
write. The documented `os` and `auto` modes were therefore misleading while
`terminal` always called `/bin/sh` directly.

The usability goal is fewer repetitive approvals inside a bounded workspace,
not a claim that prompts alone form a boundary. Anthropic reported that Claude
Code sandboxing reduced permission prompts by 84% in internal use while
enforcing filesystem and network boundaries
([Beyond permission prompts](https://www.anthropic.com/engineering/claude-code-sandboxing),
2025-10-20). tny retains its independent permission decision and uses the OS
wrapper as defense in depth after that decision.

## Decision

Only a local native-loop `terminal` child is wrapped. The tny process and
session runner remain outside, as do built-in file tools, MCP, extensions,
web-provider I/O, and tools owned by Cursor, Codex, or ACP hosts. Both
foreground and `background:true` terminal commands take the same path. SSH is
unchanged: the remote host is the boundary, so tny does not attempt to install
or invoke a remote wrapper.

On macOS, tny executes `/usr/bin/sandbox-exec -p PROFILE`. This was verified on
Darwin 27 arm64 and keeps the profile/argv constructor independently testable;
linking the private/deprecated `sandbox_init` interface would add a less stable
runtime dependency. The default-deny Seatbelt profile permits reads, process
execution, outbound networking, localhost listeners, and writes only beneath
the workspace, configured extra directories, the effective temp directory,
and required device nodes.

On Linux, tny executes `bwrap` from `PATH`: `/` is read-only, the workspace,
extra dirs, and temp are overlaid read-write, `/dev` is minimal, `/proc` is
mounted, and the PID namespace is unshared. The network namespace is shared.
Network stays open by default on both platforms because coding commands need
package registries, test fixtures, provider CLIs, and localhost development
servers; this mode is not an egress firewall.

`auto` selects `os` only when the platform wrapper is executable, otherwise
`none`. Explicit `os` on an unsupported host is a clean tool error. Per ADR
0001, `yolo` always forces effective `none` for the process. Doctor, status,
and the TUI show the effective mode rather than echoing configuration.

Sandbox widening remains separate from command approval. There is no automatic
prompt/retry loop: a recognizable OS write denial becomes a tool error naming
the path and the extra-dir controls (`tny workspace add DIR` / `--add-dir`).

wasm has no child-process OS boundary: `auto` resolves to `none`, and explicit
`os` returns a clean unsupported error before command execution.

## Consequences

- Allowed shell commands can freely read the host filesystem but cannot write
  outside the named roots on supported local platforms.
- Network access is intentionally not contained; users needing egress control
  must compose tny with a network sandbox or remote execution boundary.
- A missing wrapper is visible and deterministic. Linux packaging/tests need
  bubblewrap in their inputs to exercise `os` rather than silently falling
  back.
- Seatbelt profile and wrapper argv construction remain allocation-heavy but
  occur only when a terminal tool runs, never on startup/help paths.
- Wrapper diagnostics are best-effort text parsing. Unknown denial wording is
  still returned verbatim with the command exit code, but cannot name a path.

## Amendment (2026-09-02): availability is probed, not assumed

The first cut treated a wrapper binary on disk as an available sandbox. CI
disproved that on every Linux runner and inside every nix build: Ubuntu 24.04
ships bubblewrap but restricts unprivileged user namespaces, and
`sandbox-exec` cannot nest inside Nix's own Seatbelt sandbox, so `auto`
wrapped the terminal child in a wrapper that exited 1 before the shell ran.
`tny_sandbox_available` now runs the wrapper once per process around
`sh -c 'exit 0'` (3 s cap, `tny_sandbox_probe`) and trusts the exit status:
a wrapper that cannot launch makes `auto` resolve to `none`, doctor reports
`none`, and an explicit `os` stays a clean error. Namespace *policy* failures
on a working wrapper (a denied write) remain execution-time tool errors.
