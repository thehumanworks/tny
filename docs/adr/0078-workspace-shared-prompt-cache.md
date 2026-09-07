# 0078 — Share prompt-cache routing across workspace tasks

Date: 2026-09-07
Status: accepted; amends routing scope in [ADR 0077](0077-openai-prompt-cache-routing.md)

## Decision

OpenAI cache routing defaults to a workspace group, including the native
tool profile and SSH host/cwd. Independent tasks, subagents, and ephemeral
asks can share the cache for their common workspace instructions. The group
is a bounded, deterministic routing ID computed with the existing FNV-1a
helper; it is not an authorization boundary or a cache lookup for responses.

The Responses `prompt_cache_key` and ChatGPT `session-id` header use this
group. `thread-id` remains the unique conversation ID. Each conversation
still sends its own complete input and has its own saved transcript. The
server must match the actual prefix before reusing it. The opaque affinity
token remains confined to one user turn, including that turn's retries and
tool rounds. No prompts, context, or tools are removed or padded.

`TNY_OPENAI_CACHE_SCOPE=session` restores the original conversation-scoped
routing. Unknown scope values also use session scope. This is useful when
concurrent activity in a busy workspace causes cache contention. The
existing `TNY_OPENAI_CACHE=0` compatibility switch and first-party-only
gate remain unchanged. The implementation adds no dependencies or platform
seams and uses the same shared C code on native and wasm targets.

## Why this helps, and what it does not establish

[OpenAI's caching guide](https://developers.openai.com/api/docs/guides/prompt-caching)
recommends grouping requests that share reusable prefixes. A new tny task
previously received an unrelated routing ID despite sharing almost all its
instructions with preceding tasks. This left reuse across tasks to chance.
Sharing only the JSON cache key did not improve the tested subscription
requests; the ChatGPT routing header also needed the shared group.

The programming language does not determine whether the provider can reuse
a prefix. C can send the same JSON and protocol controls as Rust. This change
is a routing-policy improvement, not a rewrite of the event loop or session
architecture. It provides an advantage for repeated independent tasks;
it does not establish higher cache hits than Codex within one continuous
conversation.

## Measurements

[Raw sanitized evidence](../benchmarks/openai-cache-strategy-2026-09-07.json)
includes executable hashes, every retained request, correctness checks,
the unsuccessful alternatives, and a separate check of default CLI behavior.
All inference used `gpt-5.6-luna` at low effort. The reference was Codex CLI
0.154.0-alpha.3. The workspace contained a fixed fictional catalog; each
question asked for a different record. Both clients received the same
catalog and questions. Results count cached input tokens, not saved bytes
or prompt-size savings.

The controlled comparison used HTTP/SSE with original request bodies and
routing headers forwarded intact. Three rounds of eight fresh tasks per
client rotated client order; each round had a new isolated workspace.
The baseline is the per-session implementation from ADR 0077.

| Fresh tasks, 24 per client | Previous tny | Updated tny | Codex CLI |
| --- | ---: | ---: | ---: |
| Input tokens | 144,344 | 144,312 | 316,840 |
| Cached input tokens | 5,632 | 112,640 | 96,000 |
| Overall cached-input fraction | 3.9% | **78.1%** | 30.3% |
| After each workspace's first task | 4.5% | **89.2%** | 29.6% |

Updated tny exceeded Codex in all three rounds. These rates include every
miss; the overall row also includes the first task in each workspace.
Its input size was effectively unchanged from the tny baseline.

A second check bypassed the proxy and used Codex's builtin provider with
its transport left at the default. Twelve fresh tasks per client produced
correct answers throughout:

| Direct CLI check | Updated tny | Codex CLI |
| --- | ---: | ---: |
| Input tokens | 72,156 | 183,760 |
| Cached input tokens | 45,056 | 82,944 |
| Overall cached-input fraction | **62.4%** | 45.1% |

The direct check uses client-reported turn usage and has no wire-size or
transport-level observations. It corroborates the fresh-task advantage
without forcing Codex to use HTTP. Different setups and traffic produced
different absolute rates; neither table is a guaranteed service level.

The original resumed-conversation workload was also rerun: two rounds of
five turns per client. After the first turn, updated tny reached **92.6%**,
the previous tny implementation **81.0%**, and Codex **98.4%**. Codex
therefore retains the higher steady-state cached-input fraction in this
workload. A smaller prompt also has a smaller denominator, so comparisons
of percentages alone cannot establish which client captures every reusable
token most efficiently. The fresh-task tny-to-tny control avoids that issue.

## Alternatives checked

- Codex-style input items with original response-item replay reached 95.5%
  warm reuse in a small prototype, below the Codex reference.
- Persistent WebSocket continuation with `previous_response_id` worked,
  but did not produce a superior cache-hit result. Keeping response state
  on a connection does not guarantee a KV-cache hit.
- A shared `prompt_cache_key` with separate ChatGPT routing IDs reached
  zero hits in its six-task test. Sharing the routing group produced the
  improvement measured above.
- Explicit content breakpoints were rejected on Luna even with the
  Codex-style protocol. One low-effort Astra capability request was also
  rejected with HTTP 400 for `prompt_cache_options`; it returned no model
  output. No Astra inference benchmark was run.

Precise breakpoint placement and cache diagnostics available on parts of
the public API were not exposed by the tested ChatGPT subscription endpoint.
That is a provider-interface constraint, not a C limitation. Richer preserved
history and persistent transports are possible architectural improvements,
but these experiments do not justify claiming they outperform Codex here.

## Reproduction and verification

```sh
python3 tests/bench/bench_prompt_cache.py --live --scenario fresh \
  --rounds 3 --turns 8 --output /tmp/workspace-cache.json
# Select the previous policy without rebuilding:
python3 tests/bench/bench_prompt_cache.py --live --client tny \
  --cache-scope session --scenario fresh --rounds 3 --turns 8 \
  --output /tmp/session-cache.json
```

The benchmark is opt-in, isolates user configuration, checks answers, and
records numeric metadata. Real credentials are never stored in its reports.
The direct corroboration used isolated configuration with a reference to
the existing credential store; it did not copy credentials into the repo.

`make test` passed (451 unit tests, 42 integration groups), including 17
focused cache/benchmark checks. Quality gates passed with clang-tidy scoped
to the changed C file; format, strict warnings and language linters covered
the full tree. `make leaks` reported zero leaks. The stripped macOS binary
remains 883,904 bytes. Emscripten/Nix and Linux-only analysis were unavailable
locally; the existing CI coverage remains required for those targets.

The checks cover shared routing across fresh tasks, distinct conversation
identities, workspace/tool-profile separation, session-scope compatibility,
turn-token reset, retries, accounting, and compatible-provider isolation.
