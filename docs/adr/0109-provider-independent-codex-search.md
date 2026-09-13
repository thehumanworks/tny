# 0109 — Provider-independent Codex search

Status: Accepted — 2026-09-13.
Partially supersedes ADR0106's default routing for local web_search/tny web search.
Preserves ADR0107/0108 background boundaries and builtin Codex inline hosted search.

## Decision

Search backend selection is independent of the conversation provider and model.
Explicit `web_search_command` wins, then `web_search_url`. Without either override,
`web_search` and `tny web search` use the Codex/ChatGPT subscription login. Only
absence of that login selects DuckDuckGo. An API-key-only Codex CLI credential
is not a subscription login; a malformed/unreadable login, missing account id,
invalid bearer, expired/unusable login or failed hosted search is an explicit
error, never a silent switch to another search service.

`core/search_codex.c` owns one bounded read-only Responses request to the Codex
service. It uses `tny_codex_credentials`: explicit ChatGPT flag fields, then
environment, then tny's auth store, then the Codex CLI auth store. Token and
account id must be nonempty valid header values. The endpoint is exclusively
`tny_codex_service_base_url`: explicit standalone `ctx->codex_base_url`, then
`TNY_CODEX_BASE_URL`, otherwise `https://chatgpt.com/backend-api/codex`. A named
conversation profile called codex, its base URL, model, API key and extra headers
cannot redirect or authenticate this request. A web_search_url override switches
to an explicit local HTTP search provider; it never rebases the Codex bearer.

The service model defaults to `gpt-5.6-sol`, independently of the conversation's
model and its saved models.codex selection. Top-level `web_search_model` can
select another supported Codex search model; the conversation is never switched.
`web_search_timeout_seconds` defaults to 120 and accepts integers 1–300. The
request sends only the query plus fixed search instructions; no transcript,
project instructions, task, workspace content or active-provider secrets.

The wire uses POST /responses, stream:true, store:false, required instructions,
`tools:[{"type":"web_search","external_web_access":true}]`, tool_choice:required,
and low reasoning effort. It has no function tools, previous_response_id, include
list or conversation affinity. Headers are independently constructed from the
Codex bearer/account, OpenAI-Beta:responses=v1, originator:tny and content type.
The existing shared HTTP/SSE seams are reused, not a recursive agent runtime,
external Codex CLI subprocess, session or daemon.

Only a complete successful Responses terminal event containing a completed
hosted web_search_call and nonempty result text produces success. Split SSE is
supported; duplicate completion events do not duplicate output. Like the existing
native backend, body shape determines SSE versus a complete JSON Response; a
missing Content-Type from the subscription gateway does not discard valid output.
JSON succeeds only after the HTTP body completes and the same final search
checks pass. Up to 64 completed streamed items are retained by id and reconciled
with terminal output; an empty terminal output array does not discard earlier
item.done results. Repeated/final copies replace rather than duplicate items. Failed/incomplete
streams, an answer without actual search, unexpected tool items, malformed data,
HTTP failures and over-limit responses produce bounded errors without raw provider
error-body text. Response wire limit is 2 MiB; text/source buffers are bounded
before normal parent tool-result truncation. URL citations are filtered to valid
HTTP(S) links and emitted as clickable autolinks. Result provenance explicitly
identifies `Codex web search results (model: ...)`, distinct from DuckDuckGo.

Shared-service refresh mirrors normal Codex credential precedence and refreshes
only the winning auth file, using the existing atomic private-file writer. This
is its sole credential write. A service-specific refresh path polls cancellation
and the existing runner control pump during header/body waits and shares the
search deadline; other login/profile callers retain their original refresh API.
Connection establishment retains the shared transport's native timeout bounds.
Search and refresh do not widen permissions or serialize credentials into the
conversation; a completed result is ordinary tool data and checkpoints at the
existing effective-result boundary. Repeated/armed Left never replays the search.

Builtin ChatGPT-mode Codex retains its inline hosted declaration from ADR0106,
which avoids a nested search-only model call. Every other native provider invokes
the common local web_search service, and the explicit CLI always invokes it even
with Codex selected. Terminal-only profiles reach it through the existing
in-process command interception. Host-owned Cursor/ACP loops can use the same
`tny web search` CLI; tny does not claim to rewrite their private tool registry.
The standalone CLI applies local permissions/settings and ChatGPT flags without
resolving, refreshing or starting an unrelated conversation provider. Under SSH,
Codex service requests run on the local tny host; command overrides retain their
existing remote terminal behavior.

## Research and alternatives

The release-pinned Codex tool declaration is unchanged:
https://raw.githubusercontent.com/openai/codex/rust-v0.155.0-alpha.3/codex-rs/tools/src/tool_spec.rs
Retrieved 2026-09-13; SHA256
`af8b5286fb6d2eb3574c484b25076f515bca2d4350ecb5059e436966ecdd519d`.

OpenAI documents completed web_search_call items, url_citation annotations and
required tool choice when a search must actually occur:
https://developers.openai.com/api/docs/guides/tools-web-search
Public API documentation does not alone prove subscription-endpoint acceptance;
actual tny live evidence is recorded in the verification report.

Declaring OpenAI's hosted tool on Grok/other-provider wire cannot grant them that
native capability. Switching the conversation to Codex changes user selection.
A full nested tny agent would inherit unnecessary tools/state and risk recursive
search. Defaulting to DDG for every non-Codex conversation was the earlier scope
error; it ignored the user's independent Codex login. The dedicated search-only
service satisfies the clarified expectation and uses the user's Codex allowance.

## Platform and verification

Native and wasm share HTTP and SSE. Browser/wasm needs available ChatGPT credential
fields/environment and endpoint CORS; failure is explicit, not a silent DDG switch.
No process or shell dependency is added to the service. Host tool-registry and
network availability are not fabricated as universal protocol support.

The amended verification contract, exact offline/live results and publication
receipt live in docs/verification/provider-independent-search.md. Fixtures cover
separate caller/search endpoints, arbitrary caller models, credential precedence,
overrides, cancellation including refresh, malformed/failed/oversized/incomplete
streams, source links, and search-result backgrounding in both tool profiles.
