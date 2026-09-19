# ADR 0153: Environment keys, OAuth stores and atomic provider selection

Status: accepted. Date: 2026-09-19.

BYOK profiles persist environment variable names, never API-key values. Names
follow POSIX identifier syntax, including lowercase letters. Saving a name does
not require the variable to exist; setup warns when it is unset. No heuristic
can distinguish every opaque key from a valid identifier. `--api-key` setup and
stored `api_key` settings are rejected with migration guidance.

Explicit provider selection wins over settings and remembered selection.
Remembered missing gateways fail closed: silently selecting a different account
could incur unexpected charges. The user can select another provider explicitly.
Profile keys and headers never fall back to another profile. An explicit missing
`xai.api_key_env` fails closed; native Grok login is eligible only when no explicit
key source exists.

Codex credentials resolve from explicit token, environment token, tny OAuth
store, then Codex OAuth store. Auto-detection requires a usable OAuth access
token, not file existence. Explicit Codex selection with a retired key-only
store reports migration guidance. Credential readers return reason codes without
printing; frontend boundaries own diagnostics. Native login and refresh remain.

Provider switching stages only mutable provider fields while borrowing the
workspace, settings and extension manager. A failed credential or settings
validation discards the stage, leaving the live context address, credentials,
headers, model, wire, effort, tier and image policy intact. Successful resolution
publishes the stage before notifying the existing extension manager.

`TNY_GROK_BASE_URL` is a local fixture override: HTTP(S), numeric `127.0.0.1`,
optional decimal port 1–65535, and an optional path. DNS hosts, userinfo, other
schemes, malformed ports and URL controls are rejected before token refresh or
request construction. Normal public xAI Responses and subscription Chat
Completions endpoints retain separate headers. This exception does not authorize
arbitrary OAuth bearer forwarding.

Reserved Cursor/ACP public ABI constants remain numerically stable but offer no
capabilities; selecting their names returns UNSUPPORTED without starting a host.
Legacy job secrets are scrubbed, never captured or replayed as Cursor credentials.
Legacy provider attachment fails before changing the live context.

Verification uses synthetic credentials and local HTTP/OAuth mocks. It makes no
claim about live provider entitlement or inference.
