# Native search and durable background turn evidence

**Final delivery:** [the final reconciliation](background-search-delivery.md)
supersedes earlier build hashes and pending supervisor gates below. All required
local/live/review gates passed; Git publication is recorded there separately.

## Historical implementer-lane evidence

Implementer evidence for WS1–WS3, BG1–BG7 and local Q1/Q2. No commit, push, PR,
Claude invocation or additional independent review was performed by the
implementer. Supervisor-owned research, live tests and review results are linked
separately; their execution is not claimed here.

## Contract and source

- [Initial contract snapshot](background-search.initial.md) was captured before
  product changes; [binding contract](background-search.md) remains authoritative.
- The single pre-contract review completed exit0 before implementation. See
  [dispositions](background-search-contract-review.md).
- The single implementation review's findings and corrections are recorded in
  [implementation review](background-search-implementation-review.md).
- Decisions: immutable ADR0106 native search/DDG, ADR0107 runner restart/dashboard,
  ADR0108 reviewed recovery, hosted boundaries and compatibility corrections.
- Product freeze manifest: `/tmp/tny-background-search-20260913/source-freeze.json`.
  Final evidence manifest and command outcomes are appended below after checks.

## Deterministic behavioral evidence

All commands use non-login `/bin/sh`, from the repository, after sourcing
`/tmp/tny-background-search-20260913/env.sh`. This prepends actual pinned tool
installations and unsets inherited `TNY_TOOLS=terminal` for general fixtures.
Live terminal-profile tests set that profile explicitly in the supervisor lane.

| Invariant | Current focused evidence |
| --- | --- |
| WS1 native Codex selection | `test_native_search.py`: exact hosted `web_search` declaration, no same-name local function, split SSE and idempotent added/done/completed events. Settings/env shadowing and explicit search command retain local function schema. |
| WS2 DDG/overrides | `test_web_search.c`: default discovery, both query placeholders, command precedence, percent encoding, direct and `uddg` source links, entities, empty/challenge/unknown/bad-URL cases. `test_native_search.py`: explicit override and preserved fetch404/1.5MiB bounds. Real DDG/Grok evidence belongs to supervisor live tests. |
| WS3 persistence/followups | `test_native_search.py`: hosted search and annotated message saved/echoed exactly once, citations visible as Markdown, no fake local function output; ordinary Responses provider retains original echo. |
| BG1 saved boundary | `test_background_agents.py`: Left delivered while long terminal waits; effective post-extension result and first effect occur once, second tool after PID replacement; repeated Left is idempotent. Hosted combination fixture parks before pending call zero. |
| BG2 exact continuation | Multi-tool, steer, reasoning, max-step/effort/model, captured-image and extension fixtures validate the next real provider request. No added prompt; exact original ordered calls/results; final turn/step counters preserved. Context unit roundtrip verifies private IPC fields and redacted public recovery. |
| BG3 restart and reattach | Successor PID differs, same-turn reattachment streams ongoing output, dashboard quit leaves worker alive; `/agents` and Ctrl-X reattach repeatedly without a second restart. Completed live runner accepts another prompt. |
| BG4 ownership/failure | Duplicate owner rejected; no synthetic flock release. Missing executable and deterministic post-G failure recover foreground. Post-RUN/pre-consumption crash leaves a safe checkpoint consumed by explicit resume; changed endpoint and already-consumed state refused with no pending effects. Stale state uses an actual free-writer probe. Existing runner tests remain required. |
| BG5 dashboard | Empty dashboard creates no session or provider runner; done/error/stale rows use saved status and real writer liveness; completed ACP followup retains host backend. |
| BG6 permissions/cancel | Focused permission Left does not arm/deny; handoff and disk recovery retain ask and park the second permission until explicit approval; explicit cancel beats armed boundary; unattended ask-B denies promptly. |
| BG7 editing/exit | Active nonempty and idle Left edit text; empty active Left arms; no-tool completion safely opens list. Background dashboard/view exit detaches. A streaming in-process fixture verifies Left/Ctrl-X//agents reject without closing its session; other unsupported paths return explicit native-runner errors. |

Focused commands passed:

```sh
TNY=/tmp/tny-background-search-20260913/feature-build/tny python3 tests/integration/test_background_agents.py
TNY=/tmp/tny-background-search-20260913/feature-build/tny python3 tests/integration/test_native_search.py
./build/tny-test -t context_checkpoint_preserves_resolved_selection
```

Logs: `review-background.log` / `.exit`, `review-search.log` / `.exit`, and
`final-context.log` in the task directory. These use local fake credentials and
loopback providers; they are not live Codex or Grok evidence.

## Full local checks

Final status: **PASS for implementer-owned local scope**. `make test`, `make quality`,
`make leaks`, the post-guard all-unit run and three clean-build mutation checks
passed. No remaining local test failures are known. Earlier unsuccessful or
superseded runs below are retained, not relabelled as passes.

The [artifact manifest](background-search-artifacts.json) binds final source,
ADRs, release binary, check logs, branch/HEAD and changed paths. All pre-existing
ADR hashes remain unchanged. The implementation was left uncommitted on
`feat/codex-search-durable-agents`.


Final `make quality` and `make leaks` passed (exit0): `final-guard-quality.log/.exit`
and `final-guard-leaks.log/.exit`, refreshed after the last product guard. Quality includes format, clang-tidy, strict warnings,
Ruff, ShellCheck, shfmt, actionlint and JS syntax; GCC analyzer is explicitly
Linux-only. macOS leak checks report zero leaks for every included suite and
CLI smoke; the configured fork-heavy suite exclusions remain visible in the log.
A later test-only refusal fixture was separately Ruff checked and formatted.

The definitive full run uses ordinary repository build paths:
`TNY=/Users/tomas/projects/tny/build/tny make test` (`final3-test.log/.exit`).
The release and shared library were rebuilt from the frozen product. A final one-line in-process dashboard guard was compiled before integration reached the background fixture, and all 548 unit tests were rerun on the final debug binary afterward (`final-units.log/.exit`). Quality/leaks were also refreshed after the guard.
`final-openai-isolated.log/.exit` passed the existing OpenAI suite unchanged,
including the previously observed abort/stall assertion.

The attempted custom-BUILD aggregate is retained in `final2-test.log`: it
exposed fixture assumptions about the repository-relative extension host and
`build/dbg` object paths, as well as the old stream-abort assertion during
concurrent load. That aggregate is not claimed successful. Its launcher was
stopped after allowing its current fixture to finish cleanup. The earlier
main-build aggregate was superseded by the two reviewed recovery UI corrections.

Mutation verification passed: all three mutants compiled, then failed the
intended behavioral oracle; clean control background/search runs passed.
`mutations/results.json` and `final-guard-mutations-clean.log/.exit` record skipped pending
call, widened restored permission and discarded hosted-item mutations. Mutants
were built only in the isolated task-directory copy; product source stayed frozen.
The clean mutation rerun explicitly removes its isolated build cache before compiling the control, avoiding stale objects from a prior mutant. Current main release size remains 1,123,328 bytes. The full default-path aggregate passed (exit0), including all 65 integration groups. Explicit platform skips remain visible in its log.

Earlier failures remain recorded: original baseline inherited the terminal-only
tool profile/PATH and failed extension fixtures; `test-final.log` found the TUI
help overlay needed one more row after adding `/agents` (fixture height corrected).
`test-verified.log` observed the existing one-second OpenAI stream-abort assertion
receiving a stalled-stream error during concurrent heavy builds; its assertion
was not weakened. The unchanged assertion passed in the final default-path run.

## Evidence boundaries

The local host is Apple Silicon macOS. Current release is 1,123,328 stripped bytes,
within the 1.8MiB macOS gate. Linux <1.0MiB, musl, Windows/MSYS, wasm browser/CORS,
hermetic Nix and hosted CI are separate checks and are not claimed locally. Nix
manifests include the new stdlib HTTP/PTY fixtures and test-only C interposer.

See [research](background-search-research.md) and [supervisor live evidence](background-search-live.md)
for actual native ChatGPT search and Grok grok-4.6 E2E. The implementer did not
substitute models or invoke those providers. The source-freeze notice allows the
supervisor to rerun live checks on the final product before delivery.

## Demonstration commands

```sh
./build/tny --provider codex ask "Search the web for the official C11 atomics documentation and cite the source."
./build/tny web search "C11 atomics documentation"
./build/tny --provider grok --model grok-4.6
./build/tny agents
./build/tny agents --json
```

In the interactive Grok turn, leave the composer empty and press Left while a
tool runs. The armed notice precedes checkpoint/fresh exec at the completed tool
boundary; the background dashboard opens. Enter reattaches without a new prompt;
`/agents` or Ctrl-X detaches back to the list. Quit leaves the background worker
running. `tny resume ID` consumes a safe unactivated checkpoint after a handoff
failure; an already-activated or configuration-mismatched checkpoint is refused.
These commands are instructions for the supervisor/user; they were not presented
as implementer-owned live-provider evidence.
