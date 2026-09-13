# 0106 — Native hosted search and DuckDuckGo fallback

Status: Accepted — 2026-09-13.
Requirements: WS1–WS3, Q1–Q2 in docs/verification/background-search.md.
Supersedes the unconfigured-search omission in ADR0055; builds on ADR0065/0069.

## Decision

The builtin Codex ChatGPT Responses profile advertises the release-pinned
`{"type":"web_search","external_web_access":true}` tool. Detection uses the
resolved ChatGPT-mode profile, never the name alone. API-key mode, chat wire,
shadowing user profiles and explicit web_search_command/web_search_url settings
do not claim that capability. No runtime capability probe adds a round trip.
Pinned source and actual capability evidence are in background-search-research.md;
the supervisor owns live tny evidence separately from the deterministic fixtures.

Hosted search stays server-owned: web_search_call items never enter the local
function-call batch. ID-based replacement coalesces added/done/completed repeats.
The normalized start/end events describe hosted execution, and failed items
remain failed. Assistant responses_items retain raw search items and completed
annotated message items; request translation echoes those items, without a
second copy of their text. Citation annotations also produce visible Markdown
links in the streamed/stored answer. Reasoning extras retain their existing
wire shapes. Chat requests omit Responses-only extras.

An explicit command override precedes a URL override. Otherwise local web_search
uses https://html.duckduckgo.com/html/?q={query}. Queries are percent encoded;
GET uses an identifiable tny User-Agent, at most three redirects, bounded header
and body waits, and a 1 MiB response ceiling. The small parser extracts result
anchors/snippets and rejects bot challenges, unknown page shapes, HTTP failures,
truncated transfers and timeouts explicitly. A known no-results page is the only
empty-result success. No HTML parser library, runtime or external search CLI is
added. Parse the full bounded response before ordinary tool-result truncation.

`tny web search QUERY` and `tny web fetch URL` expose the same permission-aware
operations to scripts. Shell profiles advertise those verbs; the terminal tool
intercepts simple first-party calls in process, preserving session permissions,
SSH/override behavior and cancellation pumping. Hosted Codex requests omit the
same-name local function from their schema. The explicit command still uses the
local override/fallback and identifies DuckDuckGo honestly.

The Grok proxy also needs explicit object types on image_preview's object
composition branches. Add those redundant type constraints without changing
accepted tool arguments or hiding other tools; otherwise its schema validator
rejects the entire all-tools request before web search is possible.

## Alternatives and platform behavior

A configured-only tool leaves Grok without search by default. Treating all
profiles named codex as native leaks an unsupported tool onto user gateways.
Executing hosted output as a local function duplicates a provider operation.
Dropping annotations or retaining only provider-internal citation markers loses
usable sources. Echoing both raw messages and their translated text duplicates
conversation content. The chosen representation avoids these cases.

Native and wasm use the shared HTTP seam. Browser fallback/URL providers require
CORS; network or policy refusal is an explicit transport error. A shell command
override remains native-only. Hosted search works wherever the resolved ChatGPT
Responses request is accepted. Live network failures remain failures; fixtures
never contact DuckDuckGo or a real model.

## Verification

Deterministic search tests cover request selection, split SSE, repeated hosted
items, visible/persisted citations, same-session follow-up, profile shadowing,
override priority, query encoding, parser challenge/error and no-results cases.
The verification evidence file records actual runs and remaining platform/live
gates, not this decision document.
