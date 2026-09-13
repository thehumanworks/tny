# Independent implementation review — one invocation

Date: 2026-09-13. Claude Code 2.1.263, requested `--model fable --effort high`,
reported model `claude-fable-5-1`, exit0. This is the ONE independent implementation
review; the earlier pre-implementation contract review is separate. No further
independent review is requested or authorized for this task. Reviewer was read-only.

Review snapshot manifest was recorded before invocation. Documentation and final
checks were explicitly still in progress; source findings below must be reconciled
against the delivered state and all relevant fixes verified together. An exit0 is
successful review execution, NOT approval of the implementation.

## Reviewer result

## Verdict

The implementation is close but does not yet satisfy the contract. Three items block: the Left-key gate ignores the empty-composer requirement, hosted-message capture changes the follow-up wire for every Responses provider rather than only builtin Codex, and the on-disk continuation checkpoint has no reader, so the "requires recovery" failure branch promises a recovery path that does not exist. Six further medium findings should be fixed before merge. The core handoff mechanics, checkpoint boundary, fresh-exec handshake, writer-lock inheritance, credential transport, dashboard liveness, and hosted-search declaration all check out against the code.

## Findings

**1. High. Left arms handoff with a non-empty composer.** `src/tui/tui_input.c:478` gates on `turn_active`, no wizard, no popover, no overlay, no pending permission, but never on `t->input.len`. The user deliverable says "active empty-composer Left", and the binding clarification for BG7 says "Left only arms with empty composer". Contract-review disposition 7 says the opposite, so the docs conflict and the code followed the disposition. Scenario: mid-turn the user types a steer draft, presses Left to fix a typo, and instead arms an exec restart at the next tool boundary while the cursor does not move. Fix: add `!t->input.len` to the gate and reconcile disposition 7 with BG7 in the contract. Test: PTY case that types text during a tool, sends Left, asserts no "Background armed" and that the cursor moved.

**2. High. Hosted `message` items are captured for all Responses-wire providers.** `src/backends/openai/openai.c:414-424` keeps every completed `message` item, and `src/backends/openai/responses.c:118-130` then echoes the raw item with its `id` and `status` on the next request and suppresses the plain assistant text. This applies to openai, grok, claude and custom Responses endpoints, not just ChatGPT-mode Codex. That changes the follow-up wire shape for providers outside the feature and grows every session with duplicate item copies. Contract WS3 requires other providers' contract preserved. Fix: capture `message` items only when a part carries a `url_citation` annotation, or only when `tool_web_search_native` is true. Test: `--provider openai` Responses fixture asserting no `responses_items` in the saved assistant message and the original `{"role":"assistant","content":...}` echo.

**3. High. The durable `continuation` checkpoint is written but never read.** `src/core/runner.c:1310` stores it in session.json; the only other references remove it at `runner.c:632` and `runner.c:1397`. In the post-activation rollback branch at `runner.c:1381-1390` the runner broadcasts "saved continuation requires recovery", sets quit, and exits with the session still `running` plus the stale key. A later `tny resume` goes through ordinary transcript repair and synthesizes failures for the remaining calls, which BG1 explicitly forbids. Either add a consumer that restores through `tny_engine_restore` and `tny_engine_continue` on resume, or stop writing the key and state in the ADR that durability is process-level only. The failure status text must not claim a recovery that does not exist. Test: force the post-`G` branch by making the child exit after `C` and assert the documented outcome; today only the pre-spawn failure is tested.

**4. Medium. DuckDuckGo results surface redirect URLs with tracking tokens.** The real probe page in the task directory has anchors of the form `//duckduckgo.com/l/?uddg=https%3A%2F%2Fwww.man7.org...&rut=...`. `src/core/tools_web.c:174-235` prefixes `https:` and reports that redirect as the result URL, so the model receives duckduckgo.com links, not source URLs. The unit fixture in `tests/test_web_search.c` uses a direct href and does not exercise the real shape. Fix: when the host is duckduckgo.com and the path is `/l/`, percent-decode `uddg` and drop `rut`. Test: parse an anchor copied from the probe and assert the man7.org URL.

**5. Medium. `/agents` and Ctrl-X from an attached background view trigger a second exec restart.** `src/tui/tui_commands.c:589-591` and `src/tui/tui_input.c:590-593` check `turn_active` before calling `tui_agents_open`, which itself already handles `background_view` at `src/tui/tui_agents.c:43`. The attach banner says "/agents returns to the list", but mid-turn it arms another handoff and waits for another tool boundary. Fix: call `tui_agents_open` unconditionally from both. Test: attach, send `/agents`, expect the dashboard with an unchanged pid file.

**6. Medium. `web_fetch` regressions from the shared fetch rewrite.** `src/core/tools_web.c:44` turns any non-2xx into an error string where the body used to be returned with its status line. Line 50 cuts the deadline from 60 s to 20 s. Line 74 makes a body over 1 MiB a hard error where it used to be truncated and bounded. Many real pages exceed 1 MiB, so fetch now fails on them. Fix: keep truncate-and-bound behaviour for the non-raw path and reserve the hard limit for the DDG path. Test: local HTTP fixture serving 1.5 MiB and a 404 with a body.

**7. Medium. Background permission parking now applies to every `tny ask -B` job.** `src/core/runner.c:1543` marks all initial-prompt runners as background, and `runner.c:756-763` with `runner.c:1605-1612` then parks ask-mode permissions for 300 s before denying. Previously an unattended `-B` job denied immediately. Callers using `tny session --wait --timeout` will see a five-minute stall per tool. Fix: park only for handoff-origin sessions, or shorten and document. Test: `tny ask -B --permission-mode ask` fixture expecting denial within seconds.

**8. Medium. Dashboard select forces the OpenAI backend id for every row.** `src/tui/tui_agents.c:99-100` sets `backend = TNY_BK_OPENAI` and `provider_name` from the row, but `-B` sessions on cursor or ACP now carry the marker too. Reattach works because the runner owns the config, but once that runner exits and the user sends a prompt, `tui_runner_ensure` spawns an OpenAI backend for provider "cursor". Fix: resolve through `tny_resolve_backend` in both branches, or mark non-native rows view-only. Test: `tny ask -B --provider cursor` fixture, select in the dashboard, assert no OpenAI spawn.

**9. Low.**
- `tools_web.c:177` treats the bare word "captcha" anywhere in the body as a challenge, so a query about captcha libraries returns a false challenge error. Anchor on `id="challenge-form"` or `anomaly.js`.
- `openai.c:3018-3021` maps an unknown mime to `image/gif` instead of rejecting, and restore skips the OOM reserve that `tny_engine_start` allocates at `runtime.c:1618`.
- `runner.c:1365` drains and broadcasts stderr lines after `client_out` was snapshotted, so a partial write can leave a torn NDJSON line at the owner after exec. Snapshot the owner buffers after the last drain.
- Observer clients are not mapped into the child at `runner.c:1345-1348` and see EOF mid-turn. Document or map them.
- `tools_web.c:28` advertises `+https://tny.sh`; nothing else in the repo references that domain.
- `openai.c:1530` now persists reasoning summaries into every Responses session; unrelated to the feature.
- The armed status prints twice: local note plus runner broadcast.

## Residual gaps and what checked out

Not run or not present here, per the review-stage note, and to be reconciled rather than counted as failures: ADRs 0106 and 0107 do not exist yet, `docs/tui.md`, `docs/cli.md` and `docs/sessions.md` have no mention of Left, agents, or `tny web`, and quality, full suite, and size gates are pending. New tests are picked up automatically by `tests/integration/run.sh` and the Nix source include, so the manifest is fine.

Coverage gaps without proven bugs: post-activation rollback, observer drop, non-empty-composer Left, Codex native search plus handoff in one turn, and extensions-enabled handoff. On the last one, `runtime.c:815-818` restores `extension_session_started` as true, so the fresh host never receives `session_start` before tool hooks; the old host exits on stdin EOF, so there is no orphan, but hook behaviour without `session_start` is unverified.

Verified as correct by reading the code: the boundary is set in `complete_tool` after the effective result is persisted, and `park_background` runs only after the index advance and again before `finish_tool_batch`, with cancellation and permission-block excluded. The checkpoint carries step, ordered calls, failure count, steer, text, tool log, usage, images, grants, sequences and turn affinity. Restart uses `posix_spawn`, inherits the writer lock's open file description continuously and verifies it by inode plus flock, inherits the listener, and gates side effects behind the ready, go, committed, run handshake with the parent quiescing the engine and MCP before go and doing no writes after run. Credentials travel only over the socketpair and are stripped from the disk copy. Hosted search is declared only in ChatGPT-mode Codex on the Responses wire with no overrides, the local function is dropped only there, items dedupe by id, events fire once, citations are http(s) only, and the chat wire strips the private items. Dashboard liveness comes from a live lock probe, `tny agents` starts no provider, a second owner is refused, dashboard quit detaches, and cancel resolves the pid from the pid file.

## Finding dispositions

All findings were actioned after the review, without invoking another reviewer.
ADRs 0106/0107 had been written concurrently with the review snapshot; ADR0108
records the subsequent corrections without changing accepted ADR bytes. Current
full-suite outcomes and the product manifest are maintained in
[verification evidence](background-search-evidence.md).

| Finding | Disposition and focused verification |
| --- | --- |
| R1 | Fixed: `tui_input.c` requires empty composer. PTY draft `draft ac`, Left, `b` renders `draft abc` without arming; idle editing and focused permission are exercised. Contract disposition 7 corrected to match the binding clarification. |
| R2 | Fixed: `capture_hosted_item` returns outside builtin native Codex search mode. `test_native_search.py` ordinary Responses case verifies no `responses_items`, raw IDs or annotations on followup; builtin/shadowed/override cases retain their own schemas. |
| R3 | Fixed: checkpoint `_resume` metadata carries exact public context and fingerprint; `tny_checkpoint_recover`, `rn_disk_packet` and `tny_engine_restore/continue` implement consumption through `tny resume` and dashboard selection. The test-only C interposer forces post-G failure and post-RUN/pre-consumption exit. Fixtures prove foreground rollback, no repeated effects, direct prompt-free disk continuation with original steps, already-consumed refusal and changed-endpoint refusal. The recovered TUI adopts the HELLO permission mode before handling approval events; a separate recovered ask-mode fixture verifies this. Activation marks the checkpoint consumed before pending effects; ordinary start rejects any leftover checkpoint. Already-consumed arbitrary-crash state cannot be replayed and is explicitly refused. |
| R4 | Fixed: exact DDG `/l/?uddg=` URLs decode to HTTP(S) destinations; `rut` is discarded. Unit fixture includes the real relative redirect shape and a captcha query. Invalid percent/control bytes and non-HTTP destinations reject. |
| R5 | Fixed: `/agents` and Ctrl-X call `tui_agents_open`; an attached background view detaches immediately. PTY fixture returns to the list and reattaches twice during the same response, verifying unchanged PID. An additional active in-process fixture proves Left/Ctrl-X//agents reject without closing the live session. |
| R6 | Fixed: strict DDG fetch retains hard error bounds; ordinary `web_fetch` retains 60s body deadline, status/body for 404 and truncate-and-bound for oversized pages. Loopback test serves both 404 and 1.5MiB responses. |
| R7 | Fixed: `background_permissions` identifies handoff origin separately from the persisted background marker. A sensitive unattended ask-B fixture denies and finalizes within six seconds; handoff ask-mode waits for explicit approval after reattach. |
| R8 | Fixed: selected rows resolve their stored provider for both live and completed cases, retaining explicit agent configuration. `agents` applies local flags before deferred provider selection. Completed ACP fixture selects a done row, sends another prompt and observes the fake ACP answer; the unreachable OpenAI endpoint is unused. |
| R9 | Fixed precise anomaly/form detection and truthful User-Agent; invalid restored image MIME rejects; native restore allocates OOM reserves. Last owner-wire drain precedes the snapshot, and later teardown stderr is log-only, avoiding mutation of captured client buffers. Observer reconnect is documented in ADR0108/sessions docs. Unrelated final Responses reasoning persistence was restricted to native Codex search mode, and only the runner acknowledges arming. Existing transport/runner/ownership and new image fixtures remain checks; the buffer and MIME corrections were also self-inspected. |
| Extension initialization | Fresh host receives distinct `session_start(reason=background_resume)` before hooks, continuing sequence counters. Stateful extension fixture asserts initialized hooks, exactly one original user_prompt_submit, two session_start events with one resume reason, and the transformed effective result reaches the continuation request. |
| S1 | Fixed terminal status vs liveness: JSON status done, running false, live true while the completed runner remains owner-attached. Fixture then sends another prompt through the same runner successfully. Stale/free-writer rows remain honest. |
| S2 | Fixed hosted completion arms a boundary after consuming its full response, before any pending local call. Restore accepts index zero only with saved completed native hosted search plus a nonempty pending batch. Fixture records the successor PID from the first local effect and verifies one search, one local result, preserved citation items and two total provider steps. |

The focused regressions pass locally on the revised product. Local full-suite
verification and supervisor-owned live evidence remain separate gates; successful
review execution alone is not approval or proof of those gates.

## Final delivery self-check (not an additional review)

The supervising delivery lane verified the two saved Fable invocations and ran
no additional independent review. A strict request-JSON regression exposed one
redundant `type` member in image_preview's object schema. The test failed before
removing the duplicate and passed afterward; the entire final suite, required
live scenarios, quality/leaks and isolated mutation controls were then rerun.
CLI documentation was reconciled with the already-accepted empty-composer,
hosted-boundary and handoff-origin permission behavior. Final proof is in
[the delivery record](background-search-delivery.md).
