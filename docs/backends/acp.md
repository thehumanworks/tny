# ACP

Canonical: [agentclientprotocol.com](https://agentclientprotocol.com/get-started/introduction). Pin wire **`protocolVersion: 1`** and schema [schema-v1.20.0](https://github.com/agentclientprotocol/agent-client-protocol/releases/tag/schema-v1.20.0). v2 is draft — keep a version switch, do not require it. Apache-2.0, Zed + JetBrains.

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

## Client lifecycle (v1)

In v1, **`session/prompt` stays pending for the whole turn**. Completion is `{ "stopReason": "end_turn"|… }`. v2 acks `{}` and idles via `state_update` — ignore until negotiated.

1. `initialize` with `protocolVersion: 1`, client capabilities, `clientInfo` `{ name: "tny", title: "tny", version }`. Accept the agent's chosen version.
2. `authenticate` (v1 name) only if `authMethods` is non-empty. Cursor uses `methodId: "cursor_login"`; prefer pre-auth.
3. `session/new` or `session/load` / `session/resume`.
4. `session/prompt` with `ContentBlock[]`. Stream arrives as `session/update`.
5. **Always** reply to `session/request_permission` or the agent hangs.
6. `session/cancel` (notification), then wait for `stopReason: "cancelled"`.
7. `session/close`.

Do not advertise `fs` or `terminal` unless tny implements them. Do not multiplex MCP on the ACP pipes. Optional spawn table: [registry.json](https://cdn.agentclientprotocol.com/registry/v1/latest/registry.json).

Absolute paths only. Line numbers 1-based. Property keys camelCase; discriminator strings snake_case.

## Methods (agent ← client)

Baseline: `initialize`, `session/new`, `session/prompt`, `session/cancel`. Auth: v1 `authenticate` / `logout` iff advertised (`auth/login` is v2). Reply to Cursor `cursor/ask_question` and `cursor/create_plan` or that agent stalls (they are not `_`-prefixed).

fx's ACP server (v1, for parity on `tny acp`) also implements `session/load`, `session/set_config_option`, `session/set_mode`. Modes there: `ask` (approve sensitive tools), `code` (auto-review).

Reasoning effort: protocolVersion 1 has no portable knob. Newer agents expose it as a session config option (thought-level category) via `session/set_config_option`; tny's client does not consume config options yet, so `--effort` on `--provider acp` emits one status line and the agent's default applies ([ADR 0009](../adr/0009-reasoning-effort.md)). Wire it through config options when the client learns to parse them.

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
