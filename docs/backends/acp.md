# ACP

Canonical: [agentclientprotocol.com](https://agentclientprotocol.com/get-started/introduction). Pin wire **`protocolVersion: 1`** and schema [schema-v1.20.0](https://github.com/agentclientprotocol/agent-client-protocol/releases/tag/schema-v1.20.0). v2 is draft — keep a version switch, do not require it. Apache-2.0, Zed + JetBrains.

tny implements **both** sides:

| Mode | Command | Role |
| --- | --- | --- |
| Client | `tny --backend acp --agent <exe> -- <args>` | Drive other agents |
| Named client | `tny --provider acp:<name>` | Drive an agent from `settings.json` |
| Server | `tny acp` | Expose the **native** OpenAI-compatible loop to editors |

## Transport (stdio, or WebSocket for remote agents)

- Client spawns the agent. JSON-RPC 2.0, UTF-8, **one message per line**, no embedded newlines.
- stdout is protocol only. stderr is logs.
- `--agent ws://host:port` (or `wss://`) connects instead of spawning: the
  same JSON-RPC messages ride the socket, **one message per text frame**,
  no trailing newline required ([ADR 0017](../adr/0017-wasm-browser-parity.md)).
  Everything above the transport (lifecycle, permissions, sessions) is
  identical, and this is the only ACP transport in the wasm build. The
  fixture is `tests/integration/fake_acp_agent_ws.py`, a frame↔line bridge
  around the stdio fake agent.
- v2 allows JSON-RPC batches; implement read-side batching even if tny sends single messages.
- Input message size cap: 8 MiB (fx's published ACP limit — match it on `tny acp`).

## Client lifecycle (v1)

In v1, **`session/prompt` stays pending for the whole turn**. Completion is `{ "stopReason": "end_turn"|… }`. v2 acks `{}` and idles via `state_update` — ignore until negotiated.

1. `initialize` with `protocolVersion: 1`, client capabilities, `clientInfo` `{ name: "tny", title: "tny", version }`. Accept the agent's chosen version.
2. `authenticate` (v1 name) only if `authMethods` is non-empty. Cursor uses `methodId: "cursor_login"`; prefer pre-auth.
3. `session/new` or `session/load` / `session/resume`.
4. If a model was requested, apply the advertised model config option with
   `session/set_config_option` and verify the returned current value.
5. `session/prompt` with `ContentBlock[]`. Stream arrives as `session/update`.
6. **Always** reply to `session/request_permission` or the agent hangs.
7. `session/cancel` (notification), then wait for `stopReason: "cancelled"`.
8. `session/close`.

Do not advertise `fs` or `terminal` unless tny implements them. Do not multiplex MCP on the ACP pipes. Optional spawn table: [registry.json](https://cdn.agentclientprotocol.com/registry/v1/latest/registry.json).

Absolute paths only. Line numbers 1-based. Property keys camelCase; discriminator strings snake_case.

## Methods (agent ← client)

Baseline: `initialize`, `session/new`, `session/prompt`, `session/cancel`. Auth: v1 `authenticate` / `logout` iff advertised (`auth/login` is v2). Reply to Cursor `cursor/ask_question` and `cursor/create_plan` or that agent stalls (they are not `_`-prefixed).

fx's ACP server (v1, for parity on `tny acp`) also implements `session/load`, `session/set_config_option`, `session/set_mode`. Modes there: `ask` (approve sensitive tools), `code` (auto-review).

Model selection uses ACP v1 session configuration. After `session/new` or
`session/load`, tny finds the agent-advertised select option whose category is
`model` (falling back to the conventional id `model`) and sends
`session/set_config_option` before the first prompt. `--model` wins over a
saved `models["acp:<name>"]`, which wins over the named profile's `model`.
When an explicit model is unavailable, session setup fails clearly instead of
silently using the agent default. With no configured model, tny leaves the
agent default untouched.

Reasoning effort remains separate: ACP v1 has no fixed effort field. Newer
agents expose it as a session config option (`thought_level` category), but tny
does not consume that option yet, so `--effort` on an ACP provider emits one
status line and the agent default applies ([ADR 0009](../adr/0009-reasoning-effort.md)).

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
      "claude-code": {
        "command": ["npx", "-y", "@agentclientprotocol/claude-agent-acp"],
        "model": "claude-sonnet-4-6"
      },
      "gemini": { "command": ["gemini", "--acp"] },
      "cursor": { "command": ["agent", "acp"] },
      "remote": { "command": ["wss://agent.example/acp"] }
    }
  }
}
```

Select these as `--provider acp:claude-code`, `--provider acp:gemini`, and so
on; `/provider acp:claude-code` uses the same namespace in the TUI. The prefix
is deliberate: bare `claude` and `cursor` already name other tny providers.
Defining an entry does not auto-select it, while `last_provider:"acp:<name>"`
restores a previously used entry. `--provider acp --agent CMD -- args` remains
the unconfigured ad-hoc form. Commands inherit the user's environment and must
authenticate themselves; credentials do not belong in these profiles.

## `tny acp` (server, native loop)

Match [fx acp](https://fx.sh/docs/using-fx/acp):

- Launch cwd is the primary workspace. One active session and one active prompt per connection.
- `--model`, `--log-file`. Do not write diagnostics to stdout.
- Auth: fail `initialize` if no native provider credential exists.
- `session/prompt` accepts text and embedded resources. Images: document as unsupported on ACP (use TUI / `ask --image`) unless we later add them.
- MCP: only `mcpServers` supplied by the client, not `~/.tny/mcp.json` (fx behavior — copy it).

`tny --ephemeral acp` keeps each `session/new` transcript and result store in
memory until the stdio server exits. During `initialize`, it advertises
`agentCapabilities.loadSession:false`; `session/load` returns a method error
that names ephemeral mode. `session/new`, repeated prompts, cancellation, and
`session/close` remain available. The native Responses wire continues to send
`store:false`. See [ADR 0020](../adr/0020-ephemeral-sessions.md).

Negotiate protocol version honestly. Prefer v2 if we implement v2 session resume/replay; otherwise advertise v1 like fx does today.
