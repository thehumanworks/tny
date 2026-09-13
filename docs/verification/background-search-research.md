# Research and initial live capability evidence

Date: 2026-09-13. Scope: the native C11 root application. This record does
not claim that the new tny implementation has passed its final live tests.

## Native Codex web search

The current backend is tny's native Responses loop (ADR0065), not app-server.
OpenAI's release `rust-v0.155.0-alpha.3` declares `ToolSpec::WebSearch` with wire
`type: web_search` and optional `external_web_access` (live when true). The pinned
source was retrieved with HTTP 200. SHA-256: `af8b5286fb6d2eb3574c484b25076f515bca2d4350ecb5059e436966ecdd519d`.

Source: https://raw.githubusercontent.com/openai/codex/rust-v0.155.0-alpha.3/codex-rs/tools/src/tool_spec.rs

The official Responses documentation specifies `web_search_call` output items
and `url_citation` annotations with URL, title and text offsets. Search outputs
need visible, clickable citations, not just discarded backend metadata.
Source: https://developers.openai.com/api/docs/guides/tools-web-search

A real installed Codex CLI 0.155.0-alpha.3 probe using the configured ChatGPT
login, model `gpt-6-astra`, `web_search=live`, and low effort completed with exit 0.
Its JSON event stream recorded four completed hosted web-search operations for
POSIX setsid documentation (two searches and two opens), followed by a cited
answer. No local commands or repository inspection were requested. The official
POSIX page returned 403 to that probe; it reported the limitation and used a
POSIX manual reproduction rather than claiming to have read the blocked page.
This proves the hosted capability is accessible with the account, but the new
tny request/parser path still requires its own live verification.

## Grok and DuckDuckGo

The unchanged tny binary was invoked directly with `--provider grok --model
grok-4.6 ask --json --ephemeral`. Exit 0, provider `grok`, model `grok-4.6`,
output `TNY_GROK_READY`, one step and no tools. User settings were restored after
this smoke probe. Search, checkpoint restart and reattachment are separate gates.

A direct HTTP GET from this Mac to
`https://html.duckduckgo.com/html/?q=POSIX%20setsid` with User-Agent
`tny/1.0 web-search` returned HTTP 200 and ten `result__a` entries. This was
real network evidence, not a mocked fixture. It does not eliminate future bot
challenges: those must be reported as errors, not empty successful search results.
DuckDuckGo documents its non-JavaScript search alternatives at
https://duckduckgo.com/duckduckgo-help-pages/features/non-javascript

## Persistence alternatives and platform constraints

Retaining the detached foreground runner and transferring ownership would avoid
reconstruction, but the user specifically requested a restart at the next tool
boundary. The selected design must therefore perform a fresh executable boundary,
not simply relabel a disconnected worker. Completed local tool effects must not
be replayed; pending calls must not enter ordinary transcript-repair synthesis.

OpenAI's public Responses API background mode supports server-side asynchronous
responses and stream cursors (`sequence_number`, `starting_after`). Its current
documentation also describes temporarily retained `store:false` background
responses. It is NOT evidence that the ChatGPT subscription endpoint supports
that mode, and it does not persist tny's local tool processes or provide a Grok
solution. Do not assert that `store:false` categorically forbids public-API
background requests. Source: https://developers.openai.com/api/docs/guides/background

POSIX limits a multi-threaded fork child to async-signal-safe operations until
exec; macOS TLS makes arbitrary fork-only execution especially relevant here.
A new worker should reuse the repository's descriptor-mapped spawn boundary
rather than carry initialized TLS state across fork.
https://pubs.opengroup.org/onlinepubs/9799919799/functions/fork.html
https://pubs.opengroup.org/onlinepubs/9799919799/functions/posix_spawn.html

`setsid` requires that the caller is not already a process-group leader. The
existing `tny_process_spawn_mapped` sets POSIX_SPAWN_SETPGROUP with pgroup 0:
a child launched through it is already a leader, so blindly calling setsid in
that child would fail. Any detached-runner spawn must account for this instead
of assuming the jobs/process helper already establishes a new session.
https://pubs.opengroup.org/onlinepubs/9799919799/functions/setsid.html

Durability claims should distinguish surviving TUI/terminal loss from automatic
recovery after machine reboot or a crash between an external side effect and
its checkpoint. Do not claim general exactly-once external side effects after
arbitrary crashes. The required no-replay proof concerns the controlled handoff.

## Baseline environment finding

The first baseline run inherited `TNY_TOOLS=terminal` from Mac Studio Exec.
`test_extensions` then correctly refused the fixture's list_files/glob_files,
and its mock rejected the untransformed error response. The complete extension
suite PASSED after unsetting that environment variable; no code changed. Final
fixture checks must use a clean runtime environment while live checks preserve
explicitly selected user behavior. Test shell: source temporary `test-env.sh`.
