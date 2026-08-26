# 0029 — Named ACP agents are namespaced providers

Date: 2026-08-26
Status: accepted

## Context

The ACP client originally required `--provider acp --agent CMD -- ARGS` on
every invocation. The ACP backend documentation showed a reusable
`settings.json` command table, but provider resolution did not consume it.
Treating every configured agent as the single provider `acp` would also mix
saved models, last-provider state, and session ownership between unrelated
hosts such as Claude, Gemini, and Cursor.

ACP schema v1.20.0 standardizes session configuration options, including the
semantic `model` category and `session/set_config_option`. tny preserved a
requested ACP model in its provider context but never consumed the options
returned by `session/new` / `session/load`, so agents silently kept their
defaults.

## Decision

`~/.tny/settings.json` accepts reusable agents at:

```json
{
  "acp": {
    "agents": {
      "NAME": {
        "command": ["executable", "arg"],
        "model": "optional-default"
      }
    }
  }
}
```

The selector is `--provider acp:NAME`. `NAME` uses letters, digits, `-`, and
`_`. `command` is a nonempty array whose members are nonempty strings; `model`,
when present, is a nonempty string. Validation is lazy and occurs only when a
profile is selected. Command strings are deep-copied into the process context
and profile-owned argv is released when switching providers.

The effective provider name remains `acp:NAME`. Existing persistence therefore
scopes `last_provider`, `models["acp:NAME"]`, and session ownership without a
second identity mechanism. Model resolution is:

```text
--model > models["acp:NAME"] > acp.agents.NAME.model
        > ACP_NAME_DEFAULT_MODEL > agent default
```

Merely defining an agent never enters automatic provider detection. A profile
may become the next default only after ordinary last-provider persistence has
recorded its use. The existing `--provider acp --agent CMD -- ARGS` form stays
the explicit ad-hoc path; combining `--agent` with `acp:NAME` is an error.

After creating or loading an ACP session, a nonempty requested model is applied
before the first prompt through the agent-advertised select option with category
`model` (or conventional id `model` when category is absent). The requested
value must be advertised and confirmed by the response. Missing, unsupported,
rejected, or unconfirmed selections fail setup clearly; an unset model leaves
the agent default untouched. The same lifecycle runs over stdio and WebSocket,
so wasm needs no additional platform seam.

## Consequences

- Multiple ACP agents can coexist without sharing saved models or sessions.
- Provider listings and selectors expose `acp:NAME` entries while the backend
  enum remains unchanged.
- A malformed unused profile is inert; selecting it reports the exact invalid
  name, command member, or model field.
- ACP model selection is portable for agents that advertise the v1.20 session
  option; older agents still run at their defaults when no model was requested.
