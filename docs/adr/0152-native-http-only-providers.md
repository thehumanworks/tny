# ADR 0152: Native OpenAI-compatible HTTP providers only

Status: accepted. Date: 2026-09-19.

## Decision

tny owns its agent loop. Its only provider implementation is the native HTTP
backend, with Responses (default) and Chat Completions (`wire_api: "chat"`)
request builders and streaming decoders. `--provider` and `--backend` select
native profiles, not external agent executables. There is no provider process
launcher, ACP client/server, Cursor SDK Bridge, Connect framing or WebSocket
transport. MCP subprocesses, terminal tools, session runners, jobs, teams and
workflows remain supported. The sibling tnytty app is unchanged.

Codex uses the ChatGPT Responses backend with native browser/device login,
OAuth token sources and refresh (ADRs 0065/0066). No Codex executable is needed.
Grok retains native device login/refresh and the compatible subscription chat
proxy, plus the public Responses API using `XAI_API_KEY`. The public wire is
confirmed by [xAI's Responses reference](https://docs.x.ai/developers/rest-api-reference/inference/responses)
(`/v1/responses`, Bearer authentication; checked 2026-09-19). No xAI runtime or SDK
is added. Subscription compatibility is covered by local wire mocks, not live
entitlement claims.

BYOK secrets come from environment variables: profiles contain `base_url` and
`api_key_env`, or use `NAME_BASE_URL` / `NAME_API_KEY`. OpenRouter and AIProxy are
generic profiles; AIProxy's URL must be explicitly configured. Header-name,
header-prefix, wire format and generic request options remain supported.
Inline/stored API-key settings fail with a migration diagnostic even if another
credential is available. SDK credential injection, in-memory credentials and
browser-tab ephemeral environment intake are retained. OAuth refresh stores
remain supported; these are distinct from persisted BYOK keys.

The built-in Claude subscription profile, token discovery and shell login
helper are removed. Explicit HTTP gateway profiles may use vendor names such
as `claude`; no Claude credentials are discovered. `CLAUDE.md` remains an
instructions alias, and optional foreign MCP configuration import remains.
ACP/Cursor selectors, their settings and process flags fail clearly. A stale
saved selector fails instead of silently choosing another provider.

## Compatibility and supersession

The public C ABI, event schema and Python/Node SDKs stay intact. Reserved
legacy provider constants retain their numeric ABI values but do not appear
in available capability masks and cannot create a runtime. Internal provider
enums and context fields may simplify. wasm uses the same HTTP loop through
fetch; local subprocess tools remain unavailable there.

This supersedes the external-provider portions of ADRs 0001, 0002, 0004, 0010,
0017, 0019, 0023, 0028, 0029, 0030-settings-schema-and-acp-map, 0032, 0044 and
0050, and ADR 0018's stored-key decision. Other behavior specified by those
ADRs remains in force. Historical ADRs and verification evidence are immutable.

## Verification

See [the verification contract](../verification/openai-only-providers/README.md).
Record baseline and final stripped artifact sizes and runtime dependencies.
Tests use synthetic HOME/environment and local mocks, never paid inference.
