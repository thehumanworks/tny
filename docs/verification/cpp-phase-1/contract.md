# Verification contract: C++ phase 1 (#137)

Created: 2026-09-16, before implementation.
Baseline: `1d8ad71d66c06c726b3c5b35e367fec678031e85`; clean integration worktree.
Platform: macOS arm64; Apple clang 21.0.0; Python 3.14.7; Node 26.8.2.
Scope authority: user requested delivery of #137 -> #138 -> #139 using GPT-6 Astra low/high subagents, separate worktrees for parallel work, verified merges to local main and cleanup after completion.
Native goal: none (`get_goal` confirmed). Active tool rules permit creation only on an explicit goal request; this ordinary delivery request does not authorize one. That higher-priority authorization rule takes precedence over the skill's default goal-linked mechanism. The complete invariant set below remains binding; no implementation or verification scope is reduced.
Workflow: verification-contract and worktrees; root coordinates all review, ADR serials, integration, hosted checks, local-main merge and cleanup. Agents cannot spawn children or merge main. Existing unrelated worktrees and live sessions are excluded.
The issue snapshot below is the complete normative specification. Its plan steps and required tests are requirements, not optional suggestions. Each P*-I* row retains its exact pass criteria. Deliver local main only when all original gates pass. PRs may supply hosted platform proof; remote main is not to be merged or pushed without separate authorization.

## Requirement and check mapping

- R1: deliver every implementation-plan item, acceptance invariant and required test in the immutable issue snapshot below; preserve explicit scope boundaries.
- C0: contract.initial.md byte equality before implementation and saved initial SHA256; source/ADR manifest captured before writing production code.
- C1: focused independent design review before implementation, different first-slice review before repeating the ownership pattern, and additional risky-boundary review.
- C2: implementation and behavior checks named in each P1-I1 through P1-I6 row; every row must pass together on the integrated source. Owners assigned in the series contract and evidence.
- C3: all named required build, quality, sanitizer, leak, fault, ABI/SDK, platform and fixture commands below; all warnings/errors resolved. Missing environments remain unmet.
- C4: all planned critical mutations below; baseline passes and intended behavioral assertion fails, then restore and rerun. Invalid mutants do not count.
- C5: startup/performance measurements below, raw samples and exact inputs retained. Phase 3 includes cumulative pre-series comparison.
- C6: immutable ADR comparison, final source/test/config/dependency manifest, requirement reconciliation, merge to local main, task-owned worktree cleanup only after completion.

## Independent reviews

Design: challenge allocator failure containment, ownership/view lifetimes, platform/build coverage and accidental scope expansion. First slice: inspect actual owned bytes/JSON/event/resource boundary with executable tests before conversion is propagated. Reviewer has fresh bounded context and does not implement or spawn agents. Root records each finding and disposition.

## Issue snapshot

## Outcome

Make provider stream parsing and tool-call assembly use explicit C++ ownership so malformed input, interrupted streams, and allocation failures cannot bypass cleanup or leave retained pointers into destroyed parse buffers. Establish the mixed C11/C++20 build once for the next two phases.

### Why this is a priority

Every native provider turn crosses this boundary. Current SSE/Connect accumulators, yyjson documents, and tool-call fragments require manual reset/free and borrowed-pointer discipline. The expectation is simpler, automatic cleanup and explicit error propagation; this is not a claim of complete memory safety or a promised parsing speedup.

## Product intent and baseline

The user prioritizes fast startup, extensibility, and reliability over the old strict executable-size ceilings. Remaining pocket-sized and smaller than a comparable fx build is desirable; size alone must not force weaker error handling. This is a scoped migration to modern C++20, not a rewrite of the whole repository.

Planning baseline: `main` at `1d8ad71d66c06c726b3c5b35e367fec678031e85` (2026-09-16). Re-inspect current main and outstanding changes before implementation. Source paths below describe that baseline; no migration or performance improvement has been demonstrated yet.

## Scope and starting points

- [SSE parser](https://github.com/thehumanworks/tny/blob/1d8ad71d66c06c726b3c5b35e367fec678031e85/src/net/sse.c), [Connect framing](https://github.com/thehumanworks/tny/blob/1d8ad71d66c06c726b3c5b35e367fec678031e85/src/net/connectrpc.c), and their internal C-facing declarations in `src/net/net.h`.
- [Tool-call assembly](https://github.com/thehumanworks/tny/blob/1d8ad71d66c06c726b3c5b35e367fec678031e85/src/backends/openai/toolcalls.c), the Chat/Responses event decoding portion of `src/backends/openai/openai.c`, and yyjson ownership helpers around `src/json/json.h`.
- Affected callers: native OpenAI/Codex profiles, `src/core/search_codex.c`, and `src/backends/cursor/rpc.c`. Inventory all callers before changing an internal signature.
- Build/test integration: `Makefile`, `.clang-tidy`, `.clang-format`, `.mise.toml`, `.github/workflows/{ci,sdk,nix,release,pages}.yml`, applicable packaging scripts, `nix/source.nix`, `nix/tests.nix`, and Nix package derivations.
- Keep vendored libraries, socket/TLS/WebSocket transports, HTTP chunk decoding, unrelated JSON consumers, ACP/MCP implementations, and `tnytty` in C. They continue to use compatible interfaces and run their existing regression suites. Do not migrate every `buf_t` call site.

## Implementation plan

1. **Record the language policy before converting production code.** Add a new ADR authorizing this limited C++20 area and amend current `AGENTS.md`, language/runtime, architecture and build documentation so agents are not instructed both to forbid C++ and implement it. Preserve finalized historical ADRs. Keep C11 for untouched C and third-party code; retain the public C ABI.
2. **Make every build lane actually compile the new code.** Introduce explicit C/C++ compiler, source, flags and linker handling; include `.cpp`/private headers in formatting, analysis, strict warnings, dependency tracking, unit/mutation/fault/sanitizer/fuzz builds and source packages. Preserve the frozen ABI0 compatibility build. Verify native CLI, shared library, SDK clean consumers, Linux glibc/musl, macOS arm64, Windows MSYS and Emscripten node/browser lanes; update hermetic Nix inputs as needed.
3. **Introduce minimal ownership types.** Use owning byte/string containers and move-only yyjson document handles with correct deleters. Keep borrowed byte views scoped to a documented synchronous parse call; copy/move retained data into owning records. Reuse the C allocator boundary through a compatible allocator/factory so C++ allocations participate in deterministic fault injection. Do not globally override the embedding application's `operator new`.
4. **Extract the actual parser work.** Move SSE/Connect accumulation, Chat/Responses event decoding and tool-call-fragment ownership behind narrow internal C-compatible facades. Keep request scheduling, tool execution, retry/checkpoint policy and the event loop in their current owners. Convert one complete decode path first, review it, then apply the pattern to the rest of the listed scope.
5. **Define error containment.** Translate allocation exceptions into existing OOM statuses/events at the private boundary before returning to C or invoking a C callback. Destructors must not throw or allocate. Never replace recoverable library failures with abort/terminate, or silently treat allocation failure as malformed JSON. Retain secret-buffer wiping and avoid extra credential copies.
6. **Reconcile size policy openly.** Add reporting for the C++ runtime and same-target stripped artifacts. In a documented new policy, replace any obsolete hard size ceiling only where the measured migration delta justifies it; retain automated size accounting and regression visibility. Do not disable size checks wholesale or claim the historical “7 MB fx” value is a current like-for-like benchmark. Freeze the revised reporting/budget policy before evaluating the candidate. Add a reproducible PTY first-prompt benchmark and automated comparison checks if the existing benchmark inventory does not provide them; bench_ttft.py alone is not first-paint evidence.

## Acceptance invariants

| ID | Observable requirement | Required proof |
| --- | --- | --- |
| P1-I1 | Converted parser/document/container owners release their resources automatically on success, reset, partial construction, malformed input, cancellation and OOM. Owning types cannot be accidentally copied. | Ownership inventory; type checks for move-only owners; ASan/UBSan and leak checks; allocation-index sweeps that include C++ allocations. Raw resource release is confined to ownership implementations/C adapters. |
| P1-I2 | Arbitrary input fragmentation preserves decoded bytes, normalized events, tool-call IDs/arguments and terminal semantics. | Existing golden fixtures plus every split position and byte-at-a-time feeds; CRLF, multiline SSE, UTF-8 splits, empty input, truncated final frames, Connect trailers/keepalives, repeated/omitted tool indices, fresh IDs and the 32-call boundary. Preserve unknown-field handling and provider reasoning payloads. |
| P1-I3 | Existing framing/payload limits and malformed-input policies remain explicit; arithmetic cannot wrap; OOM yields an explicit failure without partial success. | Limit-1/limit/limit+1 cases, checked arithmetic, malformed JSON and parse-vs-OOM checks; preserve the existing 64 MiB Connect limit. Inventory any currently unbounded consumer and document a deliberate per-consumer limit if one is introduced rather than silently imposing a universal cap. |
| P1-I4 | No C++ exception crosses C ABI/callback boundaries; libtny survives ordinary failure and can start a successful later turn. | Fault-injected real-library tests, two consecutive OOM turns followed by success, reserved ERROR plus exactly one TURN_END, unchanged exports/layouts and C/Python/Node consumers. |
| P1-I5 | New sources are included in every applicable build, quality and safety lane; all previously supported capabilities remain supported. | Positive C++ source discovery plus isolated negative checks proving a known formatting violation and a representative enabled analyzer diagnostic fail their respective gates; target matrix results, ABI/package checks and native/wasm protocol fixture results. |
| P1-I6 | Startup remains within the shared performance gate; parser throughput and peak memory do not regress by more than 10% on a fixed representative corpus without an explicitly accepted scope revision. | Before/after raw measurements using whole-frame, one-byte and deterministic fragmented input; record allocation counts and size/dependency deltas. |

## Required tests and faults

Run `make test`, `make quality`, `make leaks`/Linux `make valgrind`, `make test-abi`, `make test-sdks`, `make test-libtny-fault`, `make test-libtny-fault-sanitize`, and `make test-libtny-fuzz-smoke` on applicable hosts. Run the existing OpenAI, search and Cursor mock suites on native targets and their existing wasm-compatible portions through `TNY=build/wasm/tny`; retain the real browser smoke. No paid/live provider calls are needed.

Add documented Make targets for a focused parser fuzz driver and portable corpus smoke test, wired into the existing CI and Nix inventories, using the repository's Linux Clang/libFuzzer approach, with bounded runs/time/RSS and deterministic portable smoke seeds. Existing ABI fuzzing alone does not exercise these streams. Ensure fuzz instrumentation covers C++ implementation objects, not just the driver.

Critical mutations: remove the frame-size check; change tool-call identity precedence; retain a temporary document view instead of owning it; swallow an OOM as success. Each applicable mutation must fail a relevant behavioral test for the intended reason. Record invalid/equivalent mutants separately.

## Performance and compatibility gate

Establish the mixed-language policy and benchmark tooling in this issue for the whole series. Compare a clean pre-change baseline with the final candidate on the same recorded reference host, compiler, release flags, and input corpus. Include loaded C++ runtime dependencies in the accounting.

- Help/version: at least 100 samples per binary in three alternating batches; median below 5 ms and added median latency no more than max(0.25 ms, 10% of baseline). Record p95 and raw samples.
- PTY first prompt, before backend work: at least 20 fresh launches per binary; median below 10 ms and added median latency no more than max(0.5 ms, 10%). This is distinct from Enter-to-first-token.
- Run `tests/bench/bench_ttft.py --tny <absolute-binary> --repo <checkout> --bench tui --iters 20 --label <baseline-or-candidate>` and the `ask-stdin` variant against its local mock. Record deltas; investigate any repeatable regression over 10%.
- Report stripped native size, wasm plus glue size, dynamic dependencies, peak memory, and relevant allocation/resource counts before/after. These are measurements, not promises that changing language makes them smaller. Preserve existing platform capability behavior.

## Agent execution and completion

Before implementation, create `docs/verification/<issue-slug>/contract.md`, a full immutable `contract.initial.md` snapshot, and `evidence.md`. Map the invariant IDs below to checks; capture source revision, dirty state, tool versions and baseline failures. Apply the verification-contract workflow when available; these issue requirements remain self-contained if that local skill is unavailable.

Obtain focused independent design review before implementation and a different review of the first working ownership boundary before extending its pattern. Record findings and dispositions. Add a uniquely numbered new ADR under `docs/adr/` for material decisions; leave finalized ADRs unchanged.

Complete only with current evidence for every invariant on the integrated delivered source: commands, exit status, exact tested revision/input hashes, relevant hosted platform results, and mutation outcomes. Update test inventories when files move. Unavailable required platforms or unresolved critical failures are explicit unmet gates, not passing checks. Deliver a focused PR linking this issue, the contract, the new ADR, and evidence. Do not modify unrelated dirty work or live sessions.

## Sequencing

This is phase 1 and has no dependency on the later C++ issues. Land the working parser migration and all mixed-language gates before phases 2 and 3 reuse it. Then deliver [#138: runtime/event and async-tool ownership](https://github.com/thehumanworks/tny/issues/138), followed by [#139: runner/job resources](https://github.com/thehumanworks/tny/issues/139).

## Amendments

None.


## Delivery amendment — 2026-09-16

The current user request is to implement #137 -> #138 -> #139 in
`/Users/tomas/projects/tny`, with exactly one independent review using
`claude --model fable --effort medium -p <prompt>`, a single pass whose
feedback is dispositioned. This supersedes the prior multi-review/model
runbook, not the behavioral, performance, compatibility or failure gates.
Implementation helpers may implement bounded slices but may not review or
spawn further agents. The requested checkout is now on
`feat/cpp-ownership-137-139`; other existing worktrees, live processes and
local/remote main are preserved. No remote-main merge, issue closure or
release is authorized by this amendment. Deliver reviewable commits/PRs
with exact current evidence; unresolved gates remain explicitly unmet.
