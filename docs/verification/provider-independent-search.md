# Verification contract: provider-independent Codex search

Status: defined before implementation; pending verification.
Baseline: 7eff456b1288a2a433c8e3b35a8756684129de59. Date: 2026-09-13.
This user clarification supersedes the search-routing scope in the original
background-search contract. Backgrounding behavior itself must not change.

## Goal

Make Codex-hosted search available regardless of the active conversation provider
or model. Use DuckDuckGo by default only when there is no Codex/ChatGPT login.
Keep explicit command/URL search overrides authoritative. A logged-in Codex
failure is an error, not permission to silently route the query to another service.

## Invariants

| ID | Requirement | Required proof |
| --- | --- | --- |
| PS1 | All native-loop providers/models can use Codex-backed search; builtin ChatGPT-mode Codex keeps inline hosted search, while the others invoke the shared credential-aware service; terminal/host consumers have the same tny web search CLI. Active conversation selection is unchanged. | Grok/custom/OpenAI mocked caller roundtrips, CLI provider matrix, live Grok grok-4.6 call producing Codex search results. |
| PS2 | Only separately resolved Codex/ChatGPT credentials and trusted Codex service endpoint authenticate the search request. Never forward active-provider keys/headers/model, project instructions or transcript. Only query and fixed search instructions go upstream. | Strict two-endpoint wire assertions, auth precedence including tny and Codex CLI stores, independent model default/setting. |
| PS3 | No subscription login (including API-key-only Codex auth) selects DuckDuckGo; explicit overrides still win. Unreadable login, malformed credentials, 401/403/429/5xx, failed/incomplete streams, no actual hosted search and cancellation report errors without fallback. | Offline auth/routing unit tests and live-service mock failures. Existing DDG parser/HTTP regressions retained. |
| PS4 | Service is a bounded read-only request with only hosted web_search, no local tools, external runtimes, sessions, recursive agent loop or background worker. Split SSE preserves final text and valid source links; no partial/error response is success. | Strict request schema, arbitrary split fixtures, terminal completion, oversized/malformed response, unexpected function call fixtures and secret-echo protection. |
| PS5 | Cancellation and runner controls remain responsive during search. Left only backgrounds after the effective search result is saved, and no completed search repeats after exec. | Slow search cancellation fixture and search-plus-background fixture; existing background_agents suite. |
| PS6 | C11 quality/size/old tests remain green; architecture and CLI/backend documentation reflect the revised default. Existing ADR bytes unchanged. | Release+size, make test/quality/leaks, relevant regression oracles, new ADR and Nix/schema documentation. |
| PS7 | Commit and push the scoped correction, update PR #133; do not wait for CI or merge. | Clean state, local/remote HEAD equality and PR receipt. |

## Boundaries

A conversation model does not acquire another provider's native wire tool.
The shared tool calls a search-only Codex Responses service with an independently
selected supported Codex model and returns its cited results to the original
conversation. Builtin Codex's existing inline native hosted search remains valid.
Host-owned loops use the provider-independent CLI rather than claiming tny can
rewrite their private tool registry. Tool availability remains subject to the
user's selected profile and permissions; search does not widen authority.
Browser/wasm uses the same HTTP/SSE seam subject to CORS; no shell dependency is
added. Search-only calls consume the user's Codex allowance.

The original review records remain historical evidence for the earlier scope,
not a claim that this new routing has already been reviewed or tested. Record
actual amendment review and final verification outcomes below.

## Contract review and binding clarifications

The Fable high contract amendment review completed successfully; see
[review/dispositions](provider-independent-search-contract-review.md). The
standalone CLI always uses the shared service, including with Codex selected.
Missing account id is an invalid login, not DDG eligibility. The only credential
write is existing winning-file OAuth refresh; service refresh body/header waits
pump cancellation/runner controls within the common deadline. Shared connection
establishment retains native transport timeout bounds. Default service model is
`gpt-5.6-sol`, changed only by `web_search_model`; timeout is 120 seconds, changed
by `web_search_timeout_seconds` (1–300). Endpoint overrides are only standalone
`ctx->codex_base_url`/`TNY_CODEX_BASE_URL`. Search runs locally under SSH. Requests
force `tool_choice: required` with only live hosted web_search, store:false and
stream:true. Results identify Codex provenance. Function and terminal-intercept
paths must both prove cancellation and result-boundary restart. Existing numbered
ADR files remain immutable; the index is mutable documentation.
