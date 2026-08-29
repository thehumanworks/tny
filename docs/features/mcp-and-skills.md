# Tools, MCP, skills, subagents

Native loop only, unless noted.

## Built-in tools

Keep fx names so prompts and muscle memory transfer:

| Area | Tools |
| --- | --- |
| Files | `list_files`, `glob_files`, `grep_files`, `read_file`, `write_file`, `edit_file`, `delete_file`, `rename_file`, `copy_file`, `create_folder`, `file_info` |
| Search | `semantic_search` (lexical, not embeddings), `open_file` |
| Shell | `terminal` (fx runtime name; accept `run_command` as an alias) |
| Web | `web_search` (optional provider), `web_fetch` |
| Images | `read_image` (png/jpeg/gif/webp via magic bytes; `vision` is an alias). Tool result is a short text; pixels go out as a follow-up user `image_url` message ([ADR 0008](../adr/0008-native-loop-images.md)). `tny ask --image PATH` attaches the same shape on the first user message (max 16 flags; a 17th is exit 1) |
| Skills | `skill`, `install_skill` |
| Subagents | `subagent` (`create`, `inspect`, `message`, `relationship`, `configure`, `lifecycle`) |
| MCP | `mcp_search_tools`, `mcp_select_tool`, `mcp_features` + selected namespaced tools |
| Runtime | `ask_user_question`, `memory`, `read_tool_result` |

Large results: bounded preview + session handle; `read_tool_result` reads a byte range or literal search. Background commands persist pid, cwd, log path, detected URL.

`memory` writes `~/.tny/memories.json` only when asked. Do not inject it into every prompt. In [ephemeral mode](../adr/0020-ephemeral-sessions.md), `memory set` is rejected so a conversation cannot create durable user memory; `get` and `list` may still read existing memories.

No browser/CDP tools in v1.

## MCP client

Trusted profile only: `~/.tny/mcp.json`. **Never** load repo-local MCP files (cloning a repo must not start a server).

Transports: stdio JSONL, Streamable HTTP, legacy HTTP+SSE if needed. Protocol target: current MCP (fx documents `2026-07-28`); negotiate down.

Lazy tools: model searches then selects so huge catalogs stay out of context. Re-check permissions immediately before `tools/call`. Treat server output as untrusted data, not instructions.

Remote auth: `header_env`, `bearer_token_env`, optional OAuth from an interactive session. No literal `Authorization` values in the profile.

ACP sessions (`tny acp`): use only client-supplied `mcpServers`, not the user profile (fx rule).

tny is not an MCP server.

## Skills

Directory + `SKILL.md` (YAML frontmatter `name`, `description`). Discover metadata at startup; load body only on invoke (`$` / `/skills` / `skill` tool).

Search order (workspace upward, stop before `$HOME`): `skills/`, `.agents/skills/`, `.claude/skills/`, `.codex/skills/`, `.cursor/skills/`, `.opencode/skills/`. Then user: `~/.tny/skills/` and the same hidden names under `$HOME`. Extra dirs do not contribute skills.

Managed installs go only to `~/.tny/skills/`.

## Subagents

Child **native** sessions. One-off or persistent. Parent/child messages queued on disk so the child transcript is not dumped into the parent. Children cannot raise permission mode above the creator unless a human set it in the manager (Ctrl-X). Host backends: no tny-spawned subagents; show host task events if they exist (e.g. Cursor `cursor/task`).

Native `create`/`message` operations emit correlated `subagent_start` and
`subagent_end` extension events around the child process. A pre-tool deny or
stop occurs before the process is started. Host task/subagent events are only
advertised when the pinned adapter supplies stable identity and a real terminal
boundary.

An ephemeral parent propagates `--ephemeral` to every child process. Those children are one-shot: `create` works, but `message` and `inspect` are unavailable because no child session id or transcript is stored. The tool result says that no resumable id exists.

## Project instructions

Load `AGENTS.md` (and `CLAUDE.md` if `AGENTS.md` is absent) from `$HOME/.tny/`, launch ancestors, and the primary workspace. Narrower path wins on conflict; user text still wins over files. Tool calls can attach target-scoped `AGENTS.md` for that path. Extra dirs do not contribute instructions. `context: false` disables this.

Over `--ssh` / `/ssh` ([ADR 0040](../adr/0040-ssh-agents-md.md)) the chain is `$HOME/.tny/` (labeled as local user policy — tools do not run there) then `AGENTS.md` from the **remote** cwd. Launch-dir and ancestor files are skipped: they describe the local tree, which is not the tool workspace. The remote file is prefixed with a banner that tny itself is local and attached over SSH.
