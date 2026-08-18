# OpenAI-compatible providers

This backend is the **native harness**. tny owns the tool loop, permissions, MCP, skills, sessions, and `tny acp`.

## HTTP surface (v1)

Implement Chat Completions first. It is what Groq, OpenRouter, Together, DeepSeek, ollama, llama.cpp, vLLM, Azure OpenAI (with tweaks), and local gateways actually share.

```text
POST {base_url}/chat/completions
Authorization: Bearer <key>          # default
# or --auth-header-name / --auth-header-prefix for Azure / odd providers
```

`base_url` examples: `https://api.openai.com/v1`, `https://openrouter.ai/api/v1`, `http://127.0.0.1:11434/v1`.

Request (minimum):

```json
{
  "model": "gpt-4.1-mini",
  "messages": [
    { "role": "system", "content": "…" },
    { "role": "user", "content": "…" }
  ],
  "tools": [ { "type": "function", "function": { "name": "read_file", "parameters": { } } } ],
  "tool_choice": "auto",
  "stream": true
}
```

Stream: `text/event-stream`. Lines `data: {…}` then `data: [DONE]`. Accumulate `choices[0].delta.content` and `choices[0].delta.tool_calls` (index, id, function.name, function.arguments fragments).

Non-stream: read `choices[0].message`.

Also implement `GET {base_url}/models` for `/models` when the provider has it; otherwise show configured ids only.

## Provider quirks (handle with flags, not forks)

| Quirk | Flag / rewrite |
| --- | --- |
| Azure `api-key` header | `auth_header_name=api-key`, empty prefix |
| Old `max_tokens` vs `max_completion_tokens` | `max_tokens_field` |
| Extra `reasoning_effort` / `thinking` | pass-through JSON object in provider profile |
| Missing tools | disable tools, error clearly |
| Parallel tool calls | honor `parallel_tool_calls` when present |

Config entry:

```json
{
  "openai": {
    "base_url": "https://openrouter.ai/api/v1",
    "api_key_env": "OPENROUTER_API_KEY",
    "model": "anthropic/claude-sonnet-4.6",
    "auth_header_name": "Authorization",
    "auth_header_prefix": "Bearer "
  }
}
```

## Agent loop

1. Assemble messages: tny preamble + `AGENTS.md` chain + skill catalog (names only) + bounded history.
2. POST with built-in + selected MCP tool schemas.
3. On tool calls: permission check → execute (parallel read-only, serial writes) → append `role: tool` messages → POST again.
4. Stop on final text, step limit, cancel, or permission deny.

This is the only backend that uses [features/permissions.md](../features/permissions.md) and [features/mcp-and-skills.md](../features/mcp-and-skills.md) as the execution engine. Host backends have their own loops; tny only maps events.

## Responses API (optional, later)

`POST /v1/responses` with typed SSE (`response.output_text.delta`, `response.function_call_arguments.delta`). Useful for OpenAI-native and Codex-shaped gateways. Do not block v1 on it. Chat Completions remains the compatibility layer.

## SSE client notes

- Honor `Last-Event-ID` only if we reconnect the same request (rare).
- Read timeouts must be long (minutes) while still detecting a dead TCP.
- Treat a 401/403 as startup-class errors (exit 1). Mid-stream 5xx as run errors (exit 2) after retry if `Retry-After` says so.
