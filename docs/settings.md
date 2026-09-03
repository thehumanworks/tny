# Settings

User defaults live in `~/.tny/settings.json`. Add the published schema URL to
get validation, documentation, and completions in JSON-aware editors:

```json
{
  "$schema": "https://raw.githubusercontent.com/thehumanworks/tny/main/schemas/settings.schema.json",
  "provider": "codex",
  "model": { "codex": "gpt-5.3-codex" },
  "effort": { "codex": "xhigh", "openai": "medium" },
  "fast": { "codex": true, "openai": false },
  "permission_mode": "yolo",
  "tools": "terminal+edit"
}
```

The schema source is [`schemas/settings.schema.json`](../schemas/settings.schema.json).
Command-line flags have the highest precedence. Environment variables remain
above settings where an environment override exists. Settings are defaults,
not replacements for one-off flags.

## Task presets

Task presets are intentionally not settings keys: their instruction bodies are
discovered as Markdown files so they can be reviewed and versioned directly.
Place user presets in `~/.tny/tasks/NAME.md` or project presets in
`<workspace>/.tny/tasks/NAME.md`; project definitions take precedence. The
runtime also accepts the four built-ins (`review`, `optimizer`, `document`, and
`retro`) and the CLI selects them with `--task NAME`. Presets may contain only
the restricted frontmatter and instructions described in [cli.md](cli.md); they
cannot add credentials, endpoints, tools, MCP servers, workspace paths, or
permission/cost escalation.

## General defaults

| Setting | Accepted shape | Equivalent CLI behavior |
| --- | --- | --- |
| `provider` | provider string | `--provider NAME` |
| `model` | string, or `{ "PROVIDER": "MODEL" }` | `--model ID` |
| `effort` | string, or per-provider object | `--effort LEVEL` |
| `fast` | boolean/string, or per-provider object | `--fast` when true/`fast`/`priority`; standard tier when false/`default` |
| `permission_mode` | `ask`, `auto`, or `yolo` | `--permission-mode` |
| `tools` | `all`, `terminal+edit`, or `terminal` | Native-loop tool profile; `TNY_TOOLS` wins; default `all` ([ADR 0062](adr/0062-native-tool-profiles-advertise-and-enforce.md)) |
| `web_search_command` | shell command template with `{query}` or `{{query}}` | enables `web_search`; runs like `terminal` ([ADR 0055](adr/0055-web-search-gating-and-command-provider.md)); wins over `web_search_url` |
| `web_search_url` | URL template with `{query}` or `{{query}}` | enables `web_search`; fetched like `web_fetch` |
| `mcp.import_from` | array of `codex`, `claude`, `grok`, `cursor-agent` (`cursor` alias accepted) | `tny mcp list` (opt-in; off by default; project files for enabled sources are trusted, [ADR 0051](adr/0052-mcp-import-from-harnesses.md)) |

## MCP imports

Foreign MCP discovery is a user-controlled authority boundary and is empty by
default:

```json
{ "mcp": { "import_from": ["codex", "claude", "grok", "cursor-agent"] } }
```

Only named sources are opened. tny reads Codex `$CODEX_HOME/config.toml`
(`~/.codex/config.toml`), Claude `~/.claude.json` plus workspace `.mcp.json`,
Grok Build `~/.grok/config.toml` plus `.grok/config.toml` overlays from the
repository root through the current workspace, and cursor-agent
user/workspace `.cursor/mcp.json`.
It never writes these files. `~/.tny/mcp.json` wins collisions; among foreign
sources the first `import_from` entry wins, while each harness's project scope
wins its user scope. Missing or malformed enabled sources warn and are skipped.
Current stdio entries run under the normal `mcp:server/tool` permission path;
HTTP/SSE records appear in `tny mcp list` as `skipped: unsupported transport`.
Commands, arguments, environment values, and URLs are never emitted by the
listing.

`models` and `last_provider` are also valid. tny maintains them after provider
and model use. Explicit `provider` and `model` defaults are user-authored and
take precedence over the corresponding remembered fields.

Most provider-specific defaults can be scoped by provider. This prevents a
Codex model or paid tier from leaking into OpenAI, Cursor, or an ACP agent.
Providers that do not implement the fast capability reject an enabled `fast`
default instead of silently ignoring it.

## Cursor SDK Bridge v1.0.30

`cursor` is a trusted user-level object for the public sdk.v1 protojson
surface. Project `.tny.json` cannot provide it and is rejected if Cursor
configuration contains a credential-like key.

```json
{
  "cursor": {
    "runtime": "local",
    "state_root": "/Users/me/.tny/cursor-state",
    "local_store": { "type": "sqlite" },
    "callbacks": { "custom_tools": true, "store": true },
    "agent_options": {
      "mode": "AGENT_MODE_OPTION_PLAN",
      "tools": { "names": ["read", "grep"] },
      "disallowedTools": ["shell"],
      "local": {
        "settingSources": ["SETTING_SOURCE_ALL"],
        "sandboxOptions": { "enabled": true },
        "autoReview": true
      },
      "mcpServers": {
        "docs": { "http": { "type": "HTTP_MCP_TRANSPORT_TYPE_HTTP", "url": "https://example.invalid/mcp" } }
      },
      "agents": {
        "reviewer": { "description": "Review changes", "prompt": "Inspect correctness", "inheritModel": true }
      }
    },
    "send_options": {
      "enableSteps": true,
      "mode": "AGENT_MODE_OPTION_AGENT"
    }
  }
}
```

| Field | Meaning |
| --- | --- |
| `runtime` | `auto`, `local`, or `cloud`; conflicts with an explicit opposite `agent_options` branch are rejected |
| `state_root` | Optional bridge `--state-root` and default root for tny's custom store |
| `local_store` | Bridge-wide `{ "type": "sqlite" }`, `{ "type": "jsonl", "rootDir": "…" }`, or `{ "type": "custom" }` |
| `callbacks.custom_tools` | Enable local registered-tool callbacks; defaults true |
| `callbacks.store` | Enable the authenticated tny `CallStore` service for custom stores; defaults true |
| `agent_options` | Validated `AgentOptions`: model, name, local/cloud, MCP servers, subagents, mode, tool allow/deny, sandbox/review, environment and metadata |
| `send_options` | Validated `SendOptions`: model, MCP, local force, cloud environment, deltas/steps, and mode |

tny removes configured `apiKey`/`api_key` and injects `CURSOR_API_KEY`, so a
key does not belong in settings. It overlays an explicit/discovered model,
workspace `cwd`/extra `dirs` when absent, streaming deltas, and active libtny
custom-tool definitions. Presence-sensitive fields are preserved: an empty
`tools.names` means no built-ins, while an absent `tools` means Cursor's
default set; deny wins over allow.

For cloud agents, set `agent_options.cloud.repos` and optional environment,
branch/PR, metadata, and GitHub-app fields. For local agents, set `local.cwd`
at most once (normally let tny inject it), multi-root `dirs`, setting sources,
sandbox/review, store, and custom-tool metadata. HTTP MCP authentication and
environment maps may contain secrets; settings.json is mode 0600, but
environment/host-managed credentials remain preferable and no secret is ever
printed.

Store type `custom` requires `callbacks.store:true`. `--ephemeral` strips a
persistent agent store and does not launch the custom-store callback. Custom
host tools are local-only; sensitive callbacks require yolo mode because
Cursor supplies no interactive approval round trip.

## Named OpenAI-compatible providers

Any top-level object with `base_url` defines a named OpenAI-compatible
provider:

```json
{
  "openrouter": {
    "base_url": "https://openrouter.ai/api/v1",
    "api_key_env": "OPENROUTER_API_KEY",
    "model": "anthropic/claude-sonnet-4.6"
  }
}
```

Select it with `--provider openrouter`.

## Named ACP agents

`acp` is a map of reusable agent definitions. Each entry has one `command`,
optional `args`, and an optional default `model`:

```json
{
  "acp": {
    "claude": {
      "command": "npx",
      "args": ["-y", "@agentclientprotocol/claude-agent-acp"]
    },
    "pi": {
      "command": "pi-acp"
    },
    "remote": {
      "command": "wss://agent.example/acp"
    }
  }
}
```

Select these with `--provider acp@claude`, `--provider acp@pi`, and
`--provider acp@remote`. Agent names use letters, digits, `-`, and `_`.
Commands inherit the user's environment and must authenticate themselves;
credentials do not belong in ACP profiles.

For compatibility, tny still reads the earlier `acp.agents.NAME` shape with a
`command` array and accepts the earlier `acp:NAME` selector. New files and IDE
suggestions use `acp.NAME`, `command` + `args`, and `acp@NAME`.

Named ACP profiles work in native and wasm builds. Native builds can spawn
stdio agents; wasm can use remote `ws://` or `wss://` agents and otherwise
returns the existing clean spawn-unavailable error.

## MCP server profile

MCP servers are deliberately not settings keys. Trusted definitions live in
`~/.tny/mcp.json`, never in a repository file, and support backward-compatible
stdio entries plus remote Streamable HTTP entries. The complete configuration,
authentication, JSON framing, explicit SSE non-support, and wasm behavior is documented in
[Tools, MCP, skills, subagents](features/mcp-and-skills.md#mcp-client).
