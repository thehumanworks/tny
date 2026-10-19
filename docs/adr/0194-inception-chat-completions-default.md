# ADR 0194: Direct Inception uses Chat Completions

## Decision

Keep one native backend and both wire formats. When no wire is explicit, the
exact HTTPS endpoint `api.inceptionlabs.ai:443/v1` (optional trailing slash)
uses Chat Completions. Everything else retains the prior Responses default.
Explicit CLI, provider environment and settings selections retain precedence.
This is an endpoint capability default, not a model-name heuristic or retry.

Resolve against the final URL, including CLI overrides, and freeze the resolved
wire in durable launch/subagent arguments. Do not strip model prefixes: direct
Inception model IDs and OpenRouter model IDs are different provider contracts.
Do not probe another provider, move credentials, or retry a 404 on another wire.

## Evidence and limits

The original invocation/configuration was not supplied. A loopback Chat-only
server reproduces a 404 from the default `/responses` route and succeeds with
explicit `--wire-api chat`, including fragmented tool arguments and tool-result
continuation. This establishes the failure mode, not the user's exact cause.

The official [Inception models page](https://www.inceptionlabs.ai/models) and
[tool-calling cookbook](https://docs.inceptionlabs.ai/natural-language-sql-agent-mercury-tool-calling)
confirm `/v1/chat/completions`. Reviewed 2026-10-19. The public page currently
shows Mercury 2.5; the user's `mercury-3` identifier is preserved but its live
availability/account entitlement is not verified. No live key or inference used.

Verification is compact: an endpoint/override truth table, an existing subagent
snapshot test extended for inferred defaults, and one loopback protocol fixture.
Existing Responses and Sign in with ChatGPT integration suites protect their
unchanged routes. No proof or provider-specific inference loop is introduced.

Existing provider-object `wire_api` settings and normalized `PROVIDER_WIRE_API`
environment variables remain the persistent controls. Validate the effective
selection after CLI precedence; malformed settings/types and unknown values
fail without echoing values, but an explicit valid override can repair them.
Unselected provider configuration must not affect the active route.
