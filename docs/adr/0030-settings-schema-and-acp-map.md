# 0030 — settings defaults are schema-backed; ACP agents use `acp@NAME`

Date: 2026-08-26
Status: accepted (amends ADR 0010, ADR 0015, and ADR 0029)

## Context

`settings.json` already carried permission mode, per-provider models, effort,
provider profiles, extension options, workspace rules, and reusable ACP
commands. The accepted surface was distributed across documentation and code,
so editors could not validate the file or suggest fields. Common command-line
choices such as provider, a global/per-provider model, and the fast tier were
not all available as user-authored defaults.

ADR 0029 added multiple ACP agents under `acp.agents.NAME`, selected as
`acp:NAME`, with the full argv encoded as one `command` array. The extra
`agents` nesting and colon selector are harder to discover and differ from the
requested command + args model used by process APIs.

## Decision

- Publish the Draft 2020-12 schema at `schemas/settings.schema.json`. Users add
  its raw GitHub URL as the top-level `$schema` property for IDE completion.
- Accept top-level defaults for `provider`, `model`, `effort`, `fast`, and
  `permission_mode`. `model`, `effort`, and `fast` can be scalar defaults or
  objects keyed by effective provider name. CLI flags remain strongest; known
  environment overrides remain above settings.
- `fast` accepts boolean or `fast|priority|default|off`. An enabled default on
  a provider without `TNY_CAP_FAST` is a startup error, not a silent no-op.
- The canonical ACP shape is `acp.NAME` with a string `command`, optional
  string-array `args`, and optional `model`. The canonical selector is
  `acp@NAME`; the effective provider name keeps that spelling so saved models
  and sessions remain isolated.
- Continue reading ADR 0029's `acp.agents.NAME` command-array shape and
  `acp:NAME` selector. Listings, examples, schema suggestions, and new
  persistence use the canonical form.
- Named ACP behavior is unchanged across transports: native can spawn stdio or
  connect WebSocket; wasm can use WebSocket and returns the existing clean
  error for local process spawning.

## Consequences

- JSON-aware IDEs can suggest and validate the complete supported settings
  surface without adding schema parsing to the tny binary.
- A minimal invocation can inherit provider, model, effort, and speed-tier
  defaults while one-off flags still win.
- Multiple ACP agents have a compact, direct configuration and unambiguous
  provider names such as `acp@claude` and `acp@pi`.
- Compatibility avoids breaking existing settings files and saved
  `last_provider` values.
