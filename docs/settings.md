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

## Conversation image input

`"image_input": { "codex": true, "openai": false }` configures
conversation image input per provider without replacing builtin authentication.
`true` means **configured, unverified**; `false` refuses image input before a turn
or pending-image mutation. An absent entry is **unknown** and preserves existing
explicit manual image use, but does not authorize automatic preview. Malformed
maps or more than 1,024 entries fail runtime configuration validation; provider switches recompute the
policy. This does not disable the independent image-generation provider or
make an unsupported transport capable of receiving images. See
[conversation image input](images.md#conversation-image-input-image_input).

## Prompt optimisation defaults

`optimise.provider` and `optimise.model` configure `/optimise`, Ctrl-O, and
`tny optimise` independently of the conversation. Their defaults are
`openrouter` and `inception/mercury-2.5`. Supply `OPENROUTER_API_KEY` or
configure the named provider normally. `TNY_OPTIMISE_PROVIDER` and
`TNY_OPTIMISE_MODEL` override these settings; explicit optimisation options
have the highest precedence. See [Prompt optimisation](optimisation.md).

The optimiser has no step cap and defaults to a 300-second timeout.
Set `"optimise": { "timeout_seconds": 600 }` in project `.tny.json` or
`~/.tny/settings.json` to change it. Timeout precedence is
`tny optimise --optimise-timeout N`, then `TNY_OPTIMISE_TIMEOUT` (seconds),
then project config, then user settings, then 300. The selected value must
be a positive integer from 1 to 86400 (one day); invalid values report an
error before contacting the provider. Environment and config overrides also
apply to Ctrl-O and `/optimise`. Parent step limits are not inherited.

## Task presets

Task presets are intentionally not settings keys: their instruction bodies are
discovered as Markdown files so they can be reviewed and versioned directly.
Place user presets in `~/.tny/tasks/NAME.md` or project presets in
`<workspace>/.tny/tasks/NAME.md`; project definitions take precedence. The
runtime also accepts the built-ins (`review`, `optimizer`, `document`, `retro`, and
`task-creation`) and the CLI selects them with `--task NAME`. Presets may contain only
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
| `self_improve` | boolean, default `true` | Default automatic recovery learning; `TNY_SELF_IMPROVE=0` or `--no-self-improve` disables it ([ADR 0154](adr/0154-default-automatic-workflow-learning.md)) |
| `tools` | `all`, `terminal+edit`, or `terminal` | Native-loop tool profile; `TNY_TOOLS` wins; default `all` ([ADR 0062](adr/0062-native-tool-profiles-advertise-and-enforce.md)) |
| `web_search_command` | shell command template with `{query}` or `{{query}}` | overrides automatic Codex-login/DDG search; runs like `terminal` ([ADR 0055](adr/0055-web-search-gating-and-command-provider.md)); wins over `web_search_url` |
| `web_search_url` | URL template with `{query}` or `{{query}}` | overrides automatic Codex-login/DDG search; fetched like `web_fetch` |
| `web_search_model` | nonempty supported Codex model ID | Independent search service model; default `gpt-5.6-sol`, never the conversation model |
| `web_search_timeout_seconds` | integer 1–300 | Independent Codex search/refresh deadline; default 120 seconds ([ADR 0109](adr/0109-provider-independent-codex-search.md)) |
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
Codex model or paid tier from leaking into another HTTP profile.
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

An `xai` profile uses the same generic named-provider schema (no STT-specific
settings keys are needed):

```json
{
  "xai": {
    "base_url": "https://api.x.ai/v1",
    "api_key_env": "XAI_API_KEY"
  }
}
```

`api_key_env` names an environment variable. Stored `api_key` settings are
rejected: export the value, configure its environment name, then delete the
stored key. SDK in-memory credential injection and browser-tab ephemeral
environment intake remain supported. Native OAuth login stores are separate.

`tny dictate --stt-provider xai` reads independent credentials: leading
`--xai-api-key`, then `XAI_API_KEY`, then the profile's `api_key_env`, then an
existing Grok OAuth login when no explicit key source was configured.
Present empty or CR/LF-bearing credentials fail rather than falling through.
Grok credentials refresh only when an actual transcription starts; `--check`
is local-only and does not confirm entitlement. STT always uses the official
`https://api.x.ai/v1/stt` endpoint and its service-selected model, ignoring
the profile's `base_url`, model, and custom auth headers. The `base_url`
remains required to identify a named profile and still configures chat when
explicitly selected with `--provider xai`. See [Dictation](dictation.md).

## MCP server profile

MCP servers are deliberately not settings keys. Trusted definitions live in
`~/.tny/mcp.json`, never in a repository file, and support backward-compatible
stdio entries plus remote Streamable HTTP entries. The complete configuration,
authentication, JSON framing, explicit SSE non-support, and wasm behavior is documented in
[Tools, MCP, skills, subagents](features/mcp-and-skills.md#mcp-client).

## Provider migration

ACP/Cursor settings, selectors, `--agent`, `--bridge-bin` and their commands
are removed. A stale saved provider fails with a migration diagnostic.
No Claude auth files or environment artifacts select a provider. Explicit
compatible gateway profiles may be named `claude`; choose Claude models through
that gateway. `CLAUDE.md` and optional MCP imports remain supported.

AIProxy is configured as a generic profile with your supplied URL:

```sh
export AIPROXY_BASE_URL='https://your-gateway.example/v1'
export AIPROXY_API_KEY='your-key'
tny --provider aiproxy ask "hello"
```

The example URL is a placeholder, not a built-in service endpoint.

## Optional ACP client profiles

External agents use their own installed executable and account authentication.
Configure a literal executable plus argument array; tny never passes this command
through a shell and never stores the agent's account tokens:

```json
{
  "acp": {
    "claude": {"command": "claude-agent-acp", "model": "sonnet"},
    "pi": {"command": "pi-acp"}
  }
}
```

Use `tny --provider acp@claude ask "hello"` or `/provider acp@claude` in the TUI.
The pi example assumes an installed ACP adapter exposing that executable; the
ordinary pi CLI is not automatically an ACP endpoint. Legacy `acp.agents.NAME`,
command arrays, and `acp:NAME` selectors remain readable. New settings should use
`acp.NAME` and `acp@NAME`. `--model` wins over saved per-provider models and the
profile default. ACP rejects unavailable models and unsupported `--fast` settings before prompting. Requested `--effort` values
must match an advertised thought-level configuration option; otherwise setup
fails before prompting. Use `--effort default` to leave the agent default. `/model` applies on the next turn; `/models` explicitly starts
the adapter to discover its advertised catalog.

For an ad-hoc command, use `tny --agent /path/to/adapter -- arg1 arg2 -- ask "hello"`.
The first `--` starts literal adapter arguments; the second returns to tny flags
and commands. Commands are bounded to 128 arguments. Credentials belong in the
agent's environment or account store, never these arguments. Help/version and
provider selection do not spawn the adapter. The restored transport is stdio;
WebSocket URLs and browser/wasm process execution fail clearly. SSH is supported
only for adapters whose local tools and settings can be disabled safely.
See [ACP client](backends/acp.md) for protocol capabilities and differences.
