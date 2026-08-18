# ACP

Canonical: [agentclientprotocol.com](https://agentclientprotocol.com/get-started/introduction), schema [v2](https://agentclientprotocol.com/protocol/v2/schema) / [v1](https://agentclientprotocol.com/protocol/v1/overview). Apache-licensed, originated at Zed.

tny implements **both** sides:

| Mode | Command | Role |
| --- | --- | --- |
| Client | `tny --backend acp --agent <exe> -- <args>` | Drive other agents |
| Server | `tny acp` | Expose the **native** OpenAI-compatible loop to editors |

## Transport (stdio)

- Client spawns the agent. JSON-RPC 2.0, UTF-8, **one message per line**, no embedded newlines.
- stdout is protocol only. stderr is logs.
- v2 allows JSON-RPC batches; implement read-side batching even if tny sends single messages.
- Input message size cap: 8 MiB (fx's published ACP limit — match it on `tny acp`).

## Client lifecycle (v2 preferred, v1 fallback)

1. `initialize` with `protocolVersion` (try `2`, accept agent's chosen version) plus `capabilities` and `info` `{ name: "tny", title: "tny", version }`.
2. `auth/login` only if `authMethods` is non-empty. Cursor CLI advertises `cursor_login`; prefer pre-auth via `CURSOR_API_KEY` / `agent login` so TUI need not open a browser.
3. `session/new` or `session/resume` / v1 `session/load`.
4. `session/prompt` with text (and embedded resources if advertised).
5. Agent replies once when the prompt is **accepted**, then streams `session/update`.
6. Handle `session/request_permission` (and v2 elicitation) or the turn blocks.
7. `session/cancel` to interrupt. Close with `session/close`.

Absolute paths only. Line numbers 1-based. Property keys camelCase; discriminator strings snake_case.

## Methods (agent ← client)

Baseline: `initialize`, `session/new`, `session/prompt`, `session/list`, `session/resume`, `session/close`, `session/cancel` (notification). Auth: `auth/login`, `auth/logout` iff advertised.

fx's ACP server (v1, for parity on `tny acp`) also implements `session/load`, `session/set_config_option`, `session/set_mode`. Modes there: `ask` (approve sensitive tools), `code` (auto-review).

## Methods (client ← agent)

`session/update` (messages, tool calls, plans, slash-command ads, config options), permission requests, optional elicitation, optional filesystem/terminal if we advertise those capabilities.

v1 `tny` as client should **not** advertise fs/terminal execution unless we implement them. Advertising and then ignoring requests is a spec violation.

## Cursor-as-ACP (not the Cursor backend)

[Cursor CLI ACP](https://cursor.com/docs/cli/acp): `agent acp`. Extra methods: `cursor/ask_question`, `cursor/create_plan` (blocking), `cursor/update_todos`, `cursor/task`, `cursor/generate_image`. If the spawned argv is Cursor's ACP, answer the blocking methods. This is still `--backend acp`.

## Agents to discover

`tny doctor` should probe `PATH` for known ACP servers (non-exhaustive, from [ACP agents](https://agentclientprotocol.com/get-started/agents)): Gemini CLI, Claude Agent (`claude-agent-acp`), Codex ACP adapter, Cursor `agent acp`, GitHub Copilot CLI, Goose, OpenCode, Pi, Qwen Code, Cline, …

Config:

```json
{
  "acp": {
    "agents": {
      "gemini": { "command": ["gemini", "--acp"] },
      "cursor": { "command": ["agent", "acp"] }
    }
  }
}
```

## `tny acp` (server, native loop)

Match [fx acp](https://fx.sh/docs/using-fx/acp):

- Launch cwd is the primary workspace. One active session and one active prompt per connection.
- `--model`, `--log-file`. Do not write diagnostics to stdout.
- Auth: fail `initialize` if no native provider credential exists.
- `session/prompt` accepts text and embedded resources. Images: document as unsupported on ACP (use TUI / `ask --image`) unless we later add them.
- MCP: only `mcpServers` supplied by the client, not `~/.tny/mcp.json` (fx behavior — copy it).

Negotiate protocol version honestly. Prefer v2 if we implement v2 session resume/replay; otherwise advertise v1 like fx does today.
