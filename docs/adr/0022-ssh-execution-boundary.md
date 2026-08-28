# ADR 0022: `--ssh` is a remote tool runtime, not a remote tny

## Status

Accepted (2026-08-23). Supersedes the first cut of the same day, which
`exec`'d the whole tny invocation on the remote host and therefore required
`tny` to be installed there (`zsh:1: command not found: tny`).

## Context

Users want to type locally and have the agent act on another machine — read
and edit files, grep, run the tests — without installing anything on it. The
remote box is often a throwaway VM or a locked-down server with nothing but
`sshd`, a POSIX shell and coreutils.

## Decision

`--ssh user@host[:port]` keeps **tny entirely local** — config, sessions,
provider connection, TUI, permissions, MCP, skills, memory — and routes every
**workspace tool** of the native loop through one persistent OpenSSH
connection (`src/core/ssh.c`, `src/core/tools_ssh.c`):

| tool | remote mechanism |
| --- | --- |
| `list_files` | `ls -1Ap` |
| `glob_files` | `find -path` (same prune list as the local walker) |
| `grep_files` | `find … \| xargs grep -nIF[i]` (substring semantics, binaries skipped) |
| `read_file` / `read_image` | `cat`; offset/limit and image sniffing applied locally; images staged under the session dir for the provider upload |
| `write_file` / `edit_file` | content on **stdin** → temp file → `mv` (atomic); edit performs the exact-substring replacement locally after a `cat` |
| `delete_file` `create_folder` `file_info` `rename_file` `copy_file` | `rm` / `mkdir -p` / `ls -ld` / `mv` / `cp -R` |
| `semantic_search` | `grep -ci` per term, ranked |
| `terminal` | the command itself, under the same timeout/kill/truncation rules; `background` uses `nohup` with the log under `~/.tny-bg/` on the remote |
| `open_file`, `install_skill` | clean "not available over --ssh" errors |

Everything else (`memory`, `skill`, `subagent`, `mcp_*`, `web_*`,
`read_tool_result`, `ask_user_question`) stays local.

Mechanics:

- **One connection.** `ssh_connect` opens an interactive ControlMaster
  (`ControlMaster=auto`, `ControlPersist=600`, socket `~/.tny/ssh/%C`, mode
  0700) before the TUI takes the terminal, so OpenSSH can prompt for a
  password or host key exactly as it normally would. Every tool call then runs
  with `BatchMode=yes` over that socket: no re-auth, ~10–30 ms per call.
- **Remote shell agnostic.** The remote login shell receives
  `cd '<cwd>' && exec sh -c '<script>'`; scripts are POSIX sh, arguments are
  single-quoted, file content never rides the command line.
- **Remote cwd.** `--ssh-cwd DIR` (default: the login directory) is resolved
  to an absolute path once at connect; relative tool paths join it, `~`
  expands remotely. `--cwd` remains the *local* workspace used for settings,
  `.tny.json`, and session scoping.
- **Model awareness.** The system preamble replaces "Primary workspace" with
  a remote banner: the host, the remote cwd, and that the local machine is not
  the workspace (without it the model "corrects" `pwd` against the local
  path). The TUI status bar shows `ssh user@host:/dir`.
- **Project instructions follow the tool workspace** ([ADR 0040](0040-ssh-agents-md.md)):
  `$HOME/.tny/AGENTS.md` still loads (user policy, labeled local); launch-dir
  / ancestor `AGENTS.md` do not; `AGENTS.md` in the remote cwd does.
- **Permission details** for rules/prompts are the remote path or command.
- **Native loop only.** Cursor, Codex and ACP hosts own their tool loops;
  `--ssh` with those providers is refused with a pointer to an
  openai-compatible provider rather than silently running tools locally.
- **TUI**: `/ssh user@host[:port] [dir]` attaches mid-session, `/ssh off`
  detaches, bare `/ssh` reports the target.
- tny never adds `StrictHostKeyChecking=no` or similar; auth and host-key
  policy are the user's OpenSSH config.

## Consequences

- Remote requirements: `sshd` + POSIX `sh`, `ls`, `find`, `grep`, `cat`,
  `mv`, `cp`, `rm`, `mkdir`, `wc`, `sed`, `cut`, `sort`, `head`, `nohup`.
- `/undo` does not cover remote edits (the undo log is local).
- The ControlMaster lingers up to 10 min idle after tny exits (OpenSSH
  `ControlPersist`); `/ssh off` tears it down immediately.
- wasm: remote-only — the browser build has no `ssh` binary to spawn; `--ssh`
  fails at connect with a clear error.
- Coverage: `tests/test_ssh.c` runs every remote tool against a fake `ssh`
  that executes the script locally in a sandbox (quoting, stdin, timeout,
  truncation, exit codes); `tests/integration/test_ssh.py` drives a full
  `ask` tool turn through the mock provider and checks that tool calls land
  in the sandbox, not in `--cwd`, plus the refusal/error paths.
