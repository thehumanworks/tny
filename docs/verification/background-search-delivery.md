# Final delivery verification — native search and background agents

Checked 2026-09-13T09:17:56+00:00 on the user's Apple Silicon Mac Studio. **All required local
acceptance gates passed, and the feature commit was pushed with its GitHub PR
opened.** The tested product sources are unchanged by this publication record.
CI completion was not required or awaited.

## Delivered behavior

Builtin Codex exposes native hosted `web_search` with live access, preserves
hosted items and clickable source citations, and accepts a saved-session follow-up.
Other providers and explicit `tny web search` use the configured command/URL or
bounded DuckDuckGo HTML fallback. Explicit settings retain precedence.

Left with an empty composer arms a saved native runner turn. After the effective
completed tool boundary is saved, a fresh executable takes over the same turn,
without repeating completed tools or adding a prompt. Hosted searches finish
consuming their provider response before any pending local call is executed.
The shared dashboard opens after handoff and directly through `tny agents`.
It supports mid-turn owner reattachment, truthful running/completed/stale states,
noninteractive JSON, and immediate `/agents` or Ctrl-X detach from a background view.

## Required verification matrix

| Gate | Final observed evidence | Result |
| --- | --- | --- |
| WS1–WS2 | Direct tny Codex search plus same-session follow-up; saved `web_search_call` and `url_citation`; strict split-SSE and follow-up fixtures. | PASS |
| WS3 / E1 search | Direct tny Grok `grok-4.6`, terminal profile; real DuckDuckGo tool result saved, source recovered on follow-up. Override/shadowed/ordinary-provider fixtures pass. | PASS |
| BG1–BG3 | Live Grok Left during first tool; result saved once, worker PID changes; remaining work continues without a new user turn. Deterministic pending-batch/hosted/extension/steer/image/step fixtures pass. | PASS |
| BG4 | Continuous writer ownership, private bounded IPC, atomic safe checkpoint recovery, rollback and activated-checkpoint refusal are exercised by unit/PTY/fault-injection fixtures. | PASS |
| BG5–BG7 / E1 lifecycle | Original TUI killed, new dashboard reattaches during second tool; `/agents`, Ctrl-X and quit detach without cancelling; repeated owner attach and follow-up work. Negative input/cancel/permission/in-process fixtures pass. | PASS |
| Q1 build | `make -j8 release size-check`: stripped 1,123,328 bytes, macOS limit 1,887,436 bytes. | PASS |
| Q1 unit/integration | `make test`: 548 unit tests, 13,646 assertions, 65 integration groups; explicit platform skips retained. | PASS |
| Q1 quality | `make quality`: format, clang-tidy, strict warnings, Ruff, ShellCheck, shfmt, actionlint and JS syntax. GCC analyzer explicitly Linux-only. | PASS |
| Q1 leaks | `make leaks`: zero leaks in every included suite and CLI smoke; configured fork-heavy exclusions remain visible. | PASS |
| Q1 mutation | Fresh isolated build; clean background/search controls pass; skip-pending-call, widen-restored-permission and drop-hosted-items mutants compile and fail their intended assertions. | PASS |
| Q2 | ADR0106/0107/0108 and CLI/TUI/backend/session/Nix documentation reconciled; all 108 pre-existing ADR hashes unchanged. | PASS |
| R1 | Existing pre-implementation Claude Fable high contract review, session `4847f48e-02f8-48dd-b41d-88e5f6882a98`, verified successful; dispositions applied. | PASS |
| R2 | Exactly one independent Claude Fable high implementation review, session `11d12fb6-4c7c-4dbe-9474-4a64452c1dfc`; findings actioned and regressions rerun. No second independent review. | PASS |
| D1 | Successful local compilation; scoped feature commit `ab9caada7f25` pushed; [PR #133](https://github.com/thehumanworks/tny/pull/133) opened. Publication metadata follows as a docs-only commit. | PASS |

## Direct live acceptance on the delivered binary

| Scenario | Provider/model | Session | Observed result |
| --- | --- | --- | --- |
| Hosted search and saved follow-up | Codex / `gpt-6-astra` | `d501d8e00afe0b10` | Native search and citation items persisted; follow-up returns the prior source. |
| DuckDuckGo search and saved follow-up | Grok / `grok-4.6` | `7708562db2d53211` | Real results persisted using `TNY_TOOLS=terminal`; follow-up returns the prior source. |
| Restart, terminal loss and mid-turn reattach | Grok / `grok-4.6` | `d980952edff60f1b` | Worker `1606` → `1677`; one original user turn, no repeated side effects; attached follow-up succeeds. |

The lifecycle marker file contains exactly, in order:

```text
FIRST_STARTED
FIRST_DONE
SECOND_STARTED
SECOND_DONE
```

`/agents` and Ctrl-X returned immediately to the list without changing the
successor PID. The completed attached runner reported `status: done`,
`running: false`, `live: true`. Its next prompt returned `TNY_REATTACH_FOLLOWUP`.
Normal provider login resolution was used; drivers did not read, print or copy
credentials. Tests used private temporary homes/workspaces and cleaned up only
their own sessions. The user's normal settings and installed tny were not changed.

## Final corrections and honest boundaries

A delivery self-check (not another independent review) found a repeated `type`
member in the image-preview schema. A new strict JSON object-pairs check failed
on the prior binary with `duplicate request JSON key: type`, then passed after
removing the redundant member. Both code and this stricter fixture are in the
frozen final source. CLI wording was reconciled with empty-composer editing,
hosted-tool response boundaries and handoff-only permission parking.

Two supplemental Grok all-tools probes accepted the tool schema and invoked
`web_search`, but DuckDuckGo returned bot challenges. tny emitted explicit tool
errors; those probes are **not counted as successful-result acceptance**.
The required Grok terminal-profile search and follow-up passed on this same final
binary. No CAPTCHA bypass, hidden provider substitution or fabricated results
were used. Search availability depends on the configured endpoint's response.

Surviving TUI/terminal loss is verified; this is not automatic reboot execution
or a guarantee of exactly-once external effects after arbitrary worker crashes.
Unconsumed checkpoints have safe explicit recovery; already-activated ambiguous
state is refused rather than replayed. Handoff requires a saved native runner;
host-managed, ephemeral, wasm and deliberate in-process turns reject it explicitly.
Linux/musl/Windows binaries, the Linux size gate, hermetic Nix, browser CORS/wasm
and hosted CI were not run on this local Mac and are not claimed as passes.

The publication scan's one raw match was proven to be the computed SHA-256 of
`src/core/codex_auth.c`, not a credential; only that verified false positive was
baselined. The classified scan found no secrets in scoped publication files.

## Artifact binding and reproduction

- Release SHA-256: `a25ffd6f80bd9d78657b93ef1ea5759cf4804500deb9e78f8123192987399346`.
- Stripped baseline: 1,073,808 bytes; final: 1,123,328; delta: 49,520 bytes. No runtime-speed improvement is claimed.
- Frozen source/test/build selection: 626 files; aggregate SHA-256: `1392a39c02f702fa53856aac10005573813c33046de0f2f9b3251440cebe3a7b`.
- Selection: Git-tracked and unignored files under `src/`, `include/`, `tests/`, `python/`, `nix/`, `scripts/`, plus `Makefile`, `flake.nix`, `flake.lock`, `.clang-format`, `.clang-tidy`, `pyproject.toml`. Aggregate hashes compact sorted-key JSON of the path-to-SHA256 map.
- Local evidence directory: `/private/tmp/tny-background-search-20260913/delivery-final`. The redacted [artifact manifest](background-search-artifacts.json) binds commands, log hashes, live sessions, source hashes and review records. Raw provider output and credentials are not committed.

```sh
cd ~/projects/tny
make -j8 release size-check
make quality
make test
make leaks
./build/tny --provider grok --model grok-4.6
./build/tny agents
./build/tny agents --json
./build/tny --provider codex ask "Search the web for the official Rust Book ownership chapter and cite the source."
./build/tny web search "official Rust Book chapter ownership"
```

Fixture runs use the pinned installed tools and unset an inherited `TNY_TOOLS`
override only in their child environment. Live terminal-profile runs explicitly
set `TNY_TOOLS=terminal`. Live tests can legitimately fail on provider/network
errors; deterministic fixtures do not require external search or credentials.

## Publication

- Repository: `thehumanworks/tny`; branch: `feat/codex-search-durable-agents`; target: `main`.
- Tested feature commit: `ab9caada7f257afd7d37a3420739e9b280693df8` (verified present at the remote branch before recording).
- Pull request: [#133 — native Codex web search and durable background agents](https://github.com/thehumanworks/tny/pull/133).
- All feature changes were staged and committed after the successful local gates. This final publication record is a documentation-only follow-up; the 626-file tested source/test/build selection is unchanged.
- No CI wait, merge or auto-merge was requested or performed. The final conversational delivery identifies the final docs commit and verified clean/remote-equal HEAD.
