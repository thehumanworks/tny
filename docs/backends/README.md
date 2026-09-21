# Native HTTP providers

`--provider NAME` (`--backend` alias) selects a native OpenAI-compatible profile.
Every profile uses tny's tools, MCP, permissions and session store; no vendor
agent executable is needed. [ADR 0152](../adr/0152-native-http-only-providers.md)
defines the current scope.

| Profile | Wire | Credentials |
| --- | --- | --- |
| `openai` / named gateways | Responses (default) or Chat Completions | Environment key, optional custom auth header |
| `codex` | ChatGPT Responses | Native OAuth browser/device login, token env/flags, refresh stores |
| `grok` public | xAI Responses | `XAI_API_KEY` |
| `grok` subscription | Compatible chat proxy | Native device login and refresh |

[HTTP configuration](openai-compatible.md) includes OpenRouter and AIProxy.
[Codex](codex.md) documents subscription authentication. Claude models may be
selected through a configured gateway; no built-in Claude login is supported.

Both HTTP wires work on wasm through fetch subject to endpoint CORS. Codex's
browser callback listener is native-only; device login and explicit token
intake support wasm. Local process tools/MCP require native builds; remote MCP
uses HTTP on wasm. SDKs inject credentials in memory through the C ABI.

Optional [ACP clients](acp.md) restore external Claude/pi-compatible agents through
the same native tool runtime over MCP. See ADR 0164; HTTP still requires no
vendor binary. ACP server and deleted vendor backends remain absent.
