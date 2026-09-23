# 0170 — Keep Codex model discovery ahead of minimum client version gates

Date: 2026-09-23
Status: accepted; amends [0065](0065-codex-chatgpt-responses-backend.md)

## Context

The ChatGPT Codex catalog requires `GET /models?client_version=…` and filters
entries by each model's `minimal_client_version`. tny already fetches that
catalog on every `tny models` and TUI `/models` invocation. It does not keep a
CLI/TUI model cache, so a stale-while-revalidate policy would not address the
missing models.

tny had claimed Codex CLI version `0.154.0` for discovery. The backend lists
`gpt-6-sol` and `gpt-6-luna` only from `0.155.0`, so their absence was caused
by the version filter despite the fresh request. Hard-coding those model IDs
would make the next catalog change a tny release as well.

## Decision

Keep the live catalog request and normalization. Use `999.999.999` as tny's
default catalog **discovery compatibility value**, rather than pinning the
claimed version to a Codex CLI release. This value is above current minimum
version gates and lets the backend return new account-visible, listed models
without a tny update solely for a higher `minimal_client_version`.

Retain `TNY_CODEX_CLIENT_VERSION` as an explicit override. If a gateway or
future backend rejects the discovery value, users can set a version accepted
by that endpoint without rebuilding tny. Existing handling of catalog HTTP
or response-shape failures still falls back to the configured model. The
version claim is sent only on catalog discovery; it does not change turn
requests or claim that tny implements future client features.

## Consequences

- The CLI and TUI show catalog changes on their next model query. They do not
  need a cache TTL or stale-while-revalidate mechanism.
- Models still depend on account access and `visibility: list`. A displayed
  model is a discovery result, not a guarantee that a turn will succeed.
- If the backend changes its version policy, the override is the immediate
  recovery path; tny can then adapt discovery to the new protocol.
- The desktop companion's separate per-window picker cache (ADR 0168) is
  outside this CLI/TUI catalog decision and has its own Refresh action.

## Verification

Authenticated live catalog probes with the same account and build returned
six listed models for `client_version=0.154.0`, excluding `gpt-6-sol` and
`gpt-6-luna`. Both `0.155.0` and `999.999.999` returned eight listed models,
including those two. After rebuilding, the default live
`tny --provider codex models --json` returned eight models, including both.
The production change passed `make -j4`, and the synthetic Codex integration
suite passed with assertions for default discovery, the `0.154.0` override,
hidden-model filtering and normalization. `make quality` passed on macOS
(with the documented Linux analyzer skip), and `make leaks` reported zero
leaks. The full `make test` suite passed after updating a search integration
helper to navigate the non-wrapping agents dashboard in both directions.
