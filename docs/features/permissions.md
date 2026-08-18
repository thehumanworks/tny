# Permissions and sandbox

Native loop: tny enforces this. Host loop: tny only **renders** host permission requests and returns the user's decision.

## Modes

| Mode | Native behavior |
| --- | --- |
| `ask` | Prompt on unresolved sensitive tools |
| `auto` | Rules + session grants first; remaining sensitive calls may run a cheap local heuristic or the active model. If still unsure, prompt (TUI) or fail (`ask` / CI) |
| `yolo` | Skip tny permission checks and command sandbox for this process. Do not rewrite saved settings |

Default: `ask` (stricter than fx's `auto` until we have a dedicated reviewer). `TNY_PERMISSION_MODE` and `--permission-mode` override the process.

## What needs approval

Never: `list_files`, `glob_files`, `grep_files`, `read_file`, `file_info` **inside** the workspace.

Always (unless allowed by rule/grant): `write_file`, `edit_file`, `delete_file`, `rename_file`, `copy_file`, `create_folder`, `run_command`, `open_file`, `install_skill`, `vision`, any path **outside** the workspace, MCP `tools/call`.

Prompt choices: Yes / Yes and don't ask again (session grant) / No. Grants die with the session.

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

`tny ask` cannot prompt. Unresolved → exit 2 unless `--yolo` or a resolved `auto` path.

## Sandbox

Separate from permission. An allowed command still runs inside:

| Mode | Meaning |
| --- | --- |
| `os` | macOS seatbelt / Linux equivalent: writes limited to workspace, extra dirs, temp, required devices. Outbound net allowed; listen on localhost extra. |
| `none` | No tny isolation |
| `auto` | `os` if supported, else `none` |

`yolo` forces effective `none` for the process. Command approval and sandbox-widening approval are two prompts.

v1 may ship `none` + `auto`→`none` on Linux if the seatbelt port is incomplete; `doctor` must say so. Do not silently claim `os` on an unsupported host.

## Host mapping

| Host request | tny keys |
| --- | --- |
| Cursor ACP `allow-once` / `allow-always` / `reject-once` | y / a / n |
| Codex app-server allow / deny | y / n (no session grant unless Codex has one) |
| ACP v2 permission request | map advertised options onto y/a/n |

Never auto-approve host requests in `ask` mode.
