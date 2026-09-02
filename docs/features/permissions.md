# Permissions and sandbox

Native loop: tny enforces this. Host loop: tny only **renders** host permission requests and returns the user's decision.

## Modes

| Mode | Native behavior |
| --- | --- |
| `ask` | Prompt on unresolved sensitive tools |
| `auto` | Rules + session grants first; remaining sensitive calls may run a cheap local heuristic or the active model. If still unsure, prompt (TUI) or fail (`ask` / CI) |
| `yolo` | Skip tny permission checks and command sandbox for this process. Do not rewrite saved settings |

Default: **`yolo`** for every provider ([ADR 0001](../adr/0001-run-all-agents-in-yolo-mode.md)): host providers run their own loops and never hand tny a real gate, so tny does not pretend to have one. `ask`/`auto` are explicit opt-ins via `permission_mode` in settings, `TNY_PERMISSION_MODE`, or `--permission-mode`, and are only enforceable on the native loop (advisory on codex/ACP, impossible on cursor).

## What needs approval

Never: `list_files`, `glob_files`, `grep_files`, `read_file`, `file_info` **inside** the workspace.

Always (unless allowed by rule/grant): `write_file`, `edit_file`, `delete_file`, `rename_file`, `copy_file`, `create_folder`, `run_command`, `open_file`, `install_skill`, `vision`, any path **outside** the workspace, MCP `tools/call`.

Prompt choices: Yes / Yes and don't ask again (session grant) / No. Grants die with the session.

Native Python extensions observe a request only after rules and session grants
leave the effective call unresolved. They may answer `allow_once`, `deny`, or
`abstain`. The decision is correlated to that exact rewritten call and all of
its targets. Extension allow-once never creates a session grant, never changes
the configured mode, and cannot authorize a later call. Abstain preserves the
normal TUI/CLI decision path; extension-resolved requests are not shown as stale
frontend prompts.

## Verbs typed into `terminal`

A single simple `tny` verb inside a `terminal` command is dispatched
in-process and reviewed as the typed tool it stands for — `tny edit FILE` is
an `edit_file` decision on the resolved path (so `"edit"` rules and grants
apply), `tny mcp call s/t` is `mcp:s/t`, `tny memory …` is `memory`. The
prompt shows the verb ahead of the identity it resolved to, e.g.
`tny edit docs/x.md -> edit_file /repo/docs/x.md`. Anything the recogniser
does not understand stays a `bash` decision on the whole command line. See
[ADR 0063](../adr/0063-in-process-intercept-of-first-party-verbs.md) for the
exact boundary.

### Nested runs (`TNY_NESTED`)

Every child the `terminal` tool starts inherits `TNY_NESTED=1` and
`TNY_NESTED_MODE=<effective mode>`. In such a process:

- a `permission_mode` from settings is clamped to the parent's mode silently;
- an explicit `--permission-mode` or `TNY_PERMISSION_MODE` that is **wider**
  than the parent's is refused with one line on stderr and exit 1.

Width is `ask < auto < yolo`. This is what stops a `tny ask -B` the model
launches from a turn from running with more authority than the turn itself.
Narrowing is always allowed. Outside a nested run the variables are absent and
nothing changes.

## Persistent rules

Only in `~/.tny/settings.json` (global or per-workspace). Project `.tny.json` cannot grant authority.

```json
{
  "permission": {
    "*": "ask",
    "bash": { "git *": "allow", "git push *": "deny" },
    "edit": { "docs/*": "allow", "*": "deny" }
  }
}
```

Last match wins. Workspace rules beat user-global. Wildcards are glob-style, not regex.

`tny ask` cannot prompt. In opt-in `ask` mode, unresolved → exit 2; the default (`yolo`) never blocks.

## Sandbox

Separate from permission. An allowed command still runs inside:

| Mode | Meaning |
| --- | --- |
| `os` | Wrap each local `terminal` child with macOS Seatbelt or Linux bubblewrap. Files are readable, but writes are limited to the workspace, extra dirs, `$TMPDIR` (or `/tmp`), and required devices. |
| `none` | No tny isolation |
| `auto` | `os` when `/usr/bin/sandbox-exec` (macOS) or `bwrap` on `PATH` (Linux) is available; otherwise `none` |

Set the requested mode with the repo-safe `.tny.json` key, for example
`{"sandbox":"os"}`. `yolo` forces effective `none` for the process without
rewriting that file. `tny doctor` and `tny status` report the effective mode,
not merely the requested string; explicit `os` fails the tool cleanly when no
supported wrapper is available.

Only the native loop's **local `terminal` child** is wrapped. Foreground and
`background:true` commands use the same wrapper. The tny process/session
runner, built-in file tools, MCP, web fetch/search HTTP, extensions, and host
providers stay outside it. `--ssh` commands also stay outside: the remote host
is their execution boundary. Existing permission review still runs before the
child starts.

Network access remains open by default so package managers, test fixtures,
provider CLIs, and local development servers keep working. Seatbelt allows
outbound connections and localhost listeners. Bubblewrap deliberately shares
the host network namespace; it is a filesystem/process boundary, not a network
firewall. Use a separate network policy when egress control is required.

Command approval and sandbox widening remain separate decisions. This change
does not add an automatic second prompt: when the wrapper denies a write, the
tool error names the reported path and points to `tny workspace add DIR` or
`--add-dir DIR`. The user can widen and retry deliberately.

macOS uses the external `sandbox-exec -p PROFILE` wrapper, verified on Darwin
27 arm64. This keeps profile construction testable and avoids linking the
private/deprecated `sandbox_init` interface. Linux uses `bwrap` with a
read-only bind of `/`, writable bind overlays for the allowed roots, a minimal
`/dev`, `/proc`, and `--unshare-pid`.

wasm: the OS sandbox is not applicable. `auto` resolves to `none`; explicit
`os` returns a clean unsupported tool error before attempting a command.

## Host mapping

| Host request | tny keys |
| --- | --- |
| Cursor ACP `allow-once` / `allow-always` / `reject-once` | y / a / n |
| Codex app-server allow / deny | y / n (no session grant unless Codex has one) |
| ACP v2 permission request | map advertised options onto y/a/n |

Never auto-approve host requests in opt-in `ask` mode. In the default `yolo` mode host requests are accepted silently — no per-call chatter ([ADR 0001](../adr/0001-run-all-agents-in-yolo-mode.md)).
