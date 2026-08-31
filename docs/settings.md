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
  "permission_mode": "yolo"
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

`models` and `last_provider` are also valid. tny maintains them after provider
and model use. Explicit `provider` and `model` defaults are user-authored and
take precedence over the corresponding remembered fields.

Most provider-specific defaults can be scoped by provider. This prevents a
Codex model or paid tier from leaking into OpenAI, Cursor, or an ACP agent.
Providers that do not implement the fast capability reject an enabled `fast`
default instead of silently ignoring it.

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
