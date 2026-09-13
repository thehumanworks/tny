# Independent implementation review: provider-independent search

One scoped review of the clarified expectation, separate from the original
PR133 implementation review and this amendment's contract review. Claude Code
`--model fable --effort high`; reported `claude-fable-5-1`, exit 0, successful
execution, session `eee9d339-6ff7-481b-a702-cbf60e9f1b4a`. The reviewer was read-only. Successful
execution is not blanket approval: the original verdict and dispositions follow.
No second independent implementation review of this amendment was requested.

## Dispositions

| Finding | Action / evidence |
| --- | --- |
| R1 SSH regression | Fixed standalone web context to select its native tool backend before SSH setup without resolving a conversation provider. New fake-SSH test proves a command override writes only in the remote workspace, while authenticated Codex search remains local and no credentials reach SSH argv. |
| R2 ADR index | Removed the table-breaking blank line; explicitly labelled this historical index as selected decisions rather than a complete directory listing. Existing numbered ADRs unchanged. |
| R3 Late cancellation | Retained intentionally: explicit cancellation wins until the result is returned to the parent tool loop. The control pump does not treat unrelated traffic/Left as cancel. Both-profile tests prove Left preserves the request, and Ctrl-C aborts without a handoff or later model request. No silent authorization change. |
| R4 Source list bounds | Deduplicate complete URL lines. Excess source references are omitted with a count instead of failing a completed search. A large duplicate-source fixture remains successful. |
| R5 HTTP-200 JSON error | Recognize an error object and return a dedicated bounded error without raw provider text. JSON-error fixture includes a dummy bearer in the body and proves it is not echoed. |
| R6 Stream sniffing | Match native JSON-versus-SSE body selection and support a UTF-8 BOM split across reads. Unknown SSE field lines are ignored by the shared parser. BOM/unusual-field fixtures pass; logical completion is still mandatory. |
| Live/transport evidence gap | Direct live probes exposed missing Content-Type and streamed item.done results followed by an empty terminal output array. The service now retains bounded items by id, deduplicates final/repeated copies and renders only after a successful terminal event. Dedicated regressions added. Actual Grok grok-4.6 terminal/all-tool search and standalone CLI passed; final-source rerun is bound in the delivery evidence. |
| wasm CI enforcement | The existing wasm-node job now executes test_search_service.py with TNY_TEST_EXPECT_WASM=1. Shared auth/HTTP/parser/caller tests run; native signal, SSH and runner/PTY cases are explicitly skipped. A command override asserts wasm's existing unsupported result. No local wasm build is claimed: this Mac has no emcc installation. |
| Quality/full suite/leaks | Final local gates are collected after the review fixes, with source hashes checked against drift. See provider-independent-search.md and its final evidence. |
| Additional secret-diagnostic check | Controlled service refresh logs only HTTP status even with debug enabled; legacy non-service refresh behavior is unchanged. No private issuer URL is emitted by the new service path. |

## Original reviewer report

Review complete. Findings below are limited to the clarification scope (shared Codex search service, refresh cancellation, standalone CLI, routing, tests, docs) against baseline 7eff456.

## Verdict

Not approvable yet. One real regression, two doc defects, several low-severity robustness items, and the acceptance gates the contract itself requires (live gateway proof, quality/test/leak gates, wasm CI enforcement) are still open. The core design holds: auth precedence, endpoint/key/model isolation, no silent DuckDuckGo on a logged-in failure, no local tool dispatch, and the builtin Codex inline path are all correct in the code as written.

## Findings

**1. Medium — `--ssh` with `tny web` is broken by the standalone context.** `src/cli/args.c:153` (`cli_make_ctx`, `standalone_web` branch) calls `cli_ssh_attach` before any provider resolution, so `ctx->backend` is still the load default of -1 (`config.c:596`). `cli_ssh_attach` at `args.c:246` refuses anything but `TNY_BK_OPENAI`, printing "provider 'unknown' executes its own tools". At baseline, `web` resolved the provider first, so `tny --ssh host web search Q` with a `web_search_command` override ran the command remotely. ADR0109 lines 74 and 75 explicitly promise that remote behavior is retained. Fix: in the standalone branch set `ctx->backend = TNY_BK_OPENAI` before attaching (the native loop is the only thing the web CLI can use), and add a fixture or unit assertion for `--ssh` plus `web` so this cannot regress silently.

**2. Low — ADR index table is broken.** `docs/adr/README.md:94` inserts a blank line before the 0109 row, which terminates the Markdown table; the row renders as loose text. Separately, the index has no rows for 0088 through 0108, so the contract-review disposition "update the 0106 index row" cannot have been done. Remove the blank line; add the missing rows or state that the index is partial.

**3. Low — spurious "interrupted" after a completed stream.** `search_codex.c:265` checks `search_stopped` after the body loop exits with `response.complete` true. A cancel arriving in that window throws away a finished, paid-for result. Acceptable under strict cancel semantics, but if the runner control pump can set cancel on unrelated control traffic, this wastes allowance. Consider skipping the post-loop check when `response.complete` is already set.

**4. Low — source list handling.** `search_message` in `search_codex.c:70` turns a source list above roughly 60 KiB into a hard error, and repeated citations of the same URL produce duplicate lines. Both are bounded, but a hard error on a legitimately long result is a needless failure. Dedupe URLs and drop the excess with a "N more sources omitted" line instead of failing.

**5. Low — misleading message for JSON error bodies under HTTP 200.** A gateway that answers `{"error":{...}}` with status 200 is reported as "malformed Codex search event" (`search_event`, line 88). No secret echo, so not a safety issue, but a dedicated "Codex search returned an error object" message would speed diagnosis.

**6. Low — sniff differs slightly from the native backend.** `openai.c:2452` treats anything not starting with `{` as SSE. `search_codex.c:251` only accepts `: d e i r`, so an SSE body whose first field is unusual (for example `id:` is covered, but a leading UTF-8 BOM is not) fails with "unrecognized format". Consider matching the native rule exactly since the docs claim parity.

## Verified correct

- **Auth precedence** matches `tny_codex_credentials` (flag, env, tny store, Codex CLI). API-key-only Codex CLI yields `handled=false` and DuckDuckGo; unreadable store, missing account id, and CR/LF-tainted tokens are explicit errors. Unit and offline fixtures cover all four states.
- **Isolation.** Only `tny_codex_service_base_url`, the Codex bearer, and the account id reach the wire. `ctx->api_key`, `ctx->base_url`, extra headers, model, and transcript never appear. Test asserts the exact request key set and absence of the project-instruction sentinel and active key.
- **No fallback on logged-in failure.** Every 4xx/5xx, malformed, partial, incomplete, no-search, failed-tool, unexpected-tool, and oversized case returns a bounded error without contacting DuckDuckGo. The fixture asserts zero GET requests across 13 modes.
- **No recursion or local execution.** `search_event` never dispatches function calls; the `unexpected-tool` fixture proves `touch unsafe` is not run.
- **Completion proof** requires `response.status == completed`, a `web_search_call` with status completed, and nonempty text. Duplicate terminal events are idempotent. Whole-JSON bodies are only parsed after the body completes.
- **Refresh.** `refresh_post` in `codex_auth.c:333` polls cancel and deadline during header and body waits, caps the reply at 1 MiB, and the only write is `write_secret_file` on the winning file. Legacy `tny_codex_refresh_if_stale` is unchanged. Connection setup keeps the shared transport's native timeouts, as documented.
- **Terminal intercept** runs before shell or SSH dispatch (`tools.c:716`, executed at `tools.c:765`) with the runner's `tools_env`, so cancel and control pump apply and the request runs locally under SSH.
- **Cleanup.** Bearer buffer and credentials are zeroed; every buffer, parser, and connection is freed on all `goto` paths. `http_close(NULL)` is safe.
- **ABI.** `tools_env` unchanged; one new exported function; `cli_globals` is internal. No non-C11 dependencies.
- **Test wiring.** `tests/integration/run.sh` globs `test_*.py`, so `make test` and the Nix derivation pick up the new suite.

## Blockers before claiming done

1. **Live gateway acceptance is unproven.** Offline fixtures cannot show that `chatgpt.com/backend-api/codex/responses` accepts `tool_choice: required`, `store: false`, and `gpt-5.6-sol` with only a hosted `web_search` tool, or that it emits `response.status` in its terminal event. PS1's live Grok call and a captured completed `web_search_call` are required evidence.
2. **Quality, test, and leak gates** have not been run on the final tree. The `<poll.h>` include and `sigaction` use in `cmd_web.c` also need the wasm build to compile.
3. **wasm CI enforcement is missing.** AGENTS.md requires every new tool path to be enforced by the wasm job, not just documented. `ci.yml` does not run `test_search_service.py` against `build/wasm/tny`. The CLI-only subset (everything except the two PTY tests) can run there with the existing loopback fixture.
4. **Finding 1** needs a fix and a regression test.

## Final model-visible instruction reconciliation

A supervising self-check, not another independent review, found one inherited
terminal-profile prompt still saying that unconfigured search uses DuckDuckGo.
A new wire-level regression failed on that old text before the correction and
passed after it. The prompt now explicitly describes the independent Codex login
and uses DuckDuckGo only without that login. This is the seventeenth shared-service
test; the full suite, quality/leaks, live cases and compiled mutation controls are
rerun after the change and bound in the final evidence.
