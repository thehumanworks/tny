# 0077 — OpenAI prompt-cache routing and complete usage accounting

Date: 2026-09-07
Status: accepted

Routing scope is amended by [ADR 0078](0078-workspace-shared-prompt-cache.md):
workspace groups are now the default, with `thread-id` preserving each
conversation's identity and session scope available as an override. The
measurements below describe the original per-session routing implementation.

## Context

The native loop replayed the full conversation without a prompt cache key.
On the ChatGPT subscription backend it also omitted the session routing
headers and the server's per-turn affinity token. Repeated requests could
therefore reach different cache workers despite sharing an unchanged prefix.
Separately, each model response overwrote the preceding response's usage:
the reported total for a tool-heavy turn was only its final model call.
Cached-input and cache-write counts were discarded altogether.

[OpenAI's caching guide](https://developers.openai.com/api/docs/guides/prompt-caching)
describes prefix reuse and stable cache keys as routing hints, not a promise
of a hit. The installed Codex CLI source, pinned to
[rust-v0.154.0-alpha.3](https://github.com/openai/codex/blob/rust-v0.154.0-alpha.3/codex-rs/core/src/client.rs),
additionally defines `x-codex-turn-state` as a first-response token that must
be replayed during that turn and discarded before the next user turn.
The live subscription endpoint confirmed this behavior.

## Decision

1. First-party OpenAI Responses requests carry `prompt_cache_key` equal to
   the tny session ID. The key survives tool rounds, retries, and resume.
   Ephemeral sessions also have an internal ID, even though their public
   result continues to expose an empty session ID.
2. The builtin ChatGPT subscription profile sends `session-id` and
   `thread-id`, using the same ID. Keep the first usable
   `x-codex-turn-state` response header across tools and retries within one
   user turn. Clear it at turn completion and before every new send.
   It is opaque transport state, not transcript data or extension metadata.
   Ignore values of 511 bytes or more: both HTTP transports retain at most
   511 bytes per response header, so the value might have been truncated.
   Header construction and consumption remain in the existing native
   request lifecycle; no additional transport or platform seam is introduced.
3. `TNY_OPENAI_CACHE=0` disables these routing hints as a compatibility
   escape hatch. It does not turn off the upstream automatic prompt cache.
   Third-party gateways, including custom profiles merely named `codex`,
   do not receive the hints unless they use the ChatGPT profile protocol or
   an actual first-party endpoint. Chat Completions keeps its existing body.
4. Preserve instructions, tool schemas, full-history replay, compaction,
   and `store:false`. Do not introduce server-owned conversation state or
   `previous_response_id`. Public-API explicit cache controls are not sent:
   the tested subscription endpoint rejected `prompt_cache_options` and
   content breakpoints on `gpt-5.6-luna`. Routing works with the existing wire.
5. Capture usage separately for each model response. Add it once to the
   turn and session totals before the next request, including reported
   incomplete/error responses and retries. Repeated terminal events cannot
   double count. Usage events now report the complete turn's input/output
   totals and the latest request's input size; the public C ABI is unchanged.
6. Native `ask --json` and background results gain `usage`, containing
   `input_tokens`, `output_tokens`, `requests`, `cached_input_tokens`,
   `uncached_input_tokens`, and `cache_write_tokens`. `requests` counts
   responses that supplied usage. Missing cache details produce `null`,
   including a partially reported turn. With no reported usage, the entire
   field is `null`. Session totals preserve existing `in`/`out` and add
   `cached_in`, `cache_write`, and reporting counters; legacy usage remains
   intact, and its absent cache breakdown is not retroactively invented.

## Benchmark method

`tests/bench/bench_prompt_cache.py` requires `--live` and uses only generated
fictional catalog records. An isolated HOME contains fake credentials and
the synthetic workspace; the loopback forwarder alone reads the user's
current ChatGPT login. It forwards the original request body and relevant
Codex routing/protocol headers, replacing only authentication. It returns
the affinity response header intact. Reports contain only numeric usage,
timing, size, and correctness metadata. Tokens, account IDs, prompts,
responses, headers, and encrypted reasoning are never written to reports.

The comparison uses `gpt-5.6-luna`, low effort, the terminal tool profile,
and Codex CLI 0.154.0-alpha.3 with isolated configuration, web search off,
and HTTP/SSE enabled for measurement. Each round has a fresh workspace and
conversation; client order rotates. Conversation rounds ask five small
catalog questions. Tool rounds require two separate shell reads, followed
by an exact answer. Every reported result must pass correctness checks;
tny's new counters must also equal the independently observed wire usage.

Baseline: a detached worktree at
`ea18e126cb787c339010e909744ac011e4043198`, built with `make release`.

```sh
python3 tests/bench/bench_prompt_cache.py --live \
  --baseline /path/to/baseline/build/tny --tny build/tny \
  --rounds 2 --turns 5 --output /tmp/cache-conversation.json
python3 tests/bench/bench_prompt_cache.py --live \
  --baseline /path/to/baseline/build/tny --tny build/tny \
  --rounds 2 --turns 1 --scenario tools --output /tmp/cache-tools.json
```

## Measured results

The [sanitized results](../benchmarks/openai-cache-2026-09-07.json) retain
the initial measurement and a final verification with executable hashes.
Both sets are included below: four rounds per workload, with no discarded
misses. Final verification also covered defensive third-party gating and
empty-usage handling, which do not change this ChatGPT workload's wire.
Cache hit rates are token-weighted.
The conversation's warm rows exclude each round's first user turn, not
individual misses. All cold starts remain in the overall totals.

| Conversation, 4 rounds × 5 turns | Baseline tny | Updated tny | Codex CLI |
| --- | ---: | ---: | ---: |
| Input tokens, including cached | 121,420 | 121,400 | 265,180 |
| Cached input tokens | 5,632 | 84,480 | 215,808 |
| Uncached input tokens | 115,788 | 36,920 | 49,372 |
| Overall cache hit rate | 4.6% | 69.6% | 81.4% |
| Warm cache hit rate | 5.8% | 86.8% | 98.4% |
| Request body bytes | 398,464 | 399,224 | 1,131,124 |
| Median first provider event | 1,307 ms | 1,403 ms | 1,376 ms |

Updated tny used 68.1% less uncached input than baseline and 25.2% less than
Codex over the full conversation sample. Warm uncached input fell 86.0%
against baseline. Codex had the higher warm hit rate and lower warm-only
uncached input; tny's smaller initial prompt accounts for its lower overall
uncached total. This change does not shrink the prompt: it preserves reuse.
Warm hit rates varied between 81.0% and 92.6% in the two measurement sets;
cache routing improves reuse without guaranteeing it.

| Tool workload, 4 fresh turns × 3 model calls | Baseline tny | Updated tny | Codex CLI |
| --- | ---: | ---: | ---: |
| Input tokens, including cached | 73,962 | 73,928 | 160,376 |
| Cached input tokens | 0 | 39,424 | 108,032 |
| Uncached input tokens | 73,962 | 34,504 | 52,344 |
| Overall cache hit rate | 0% | 53.3% | 67.4% |
| Median first provider event | 1,480 ms | 1,560 ms | 1,399 ms |

The tool workload reduced uncached input by 53.4% against baseline and 34.1%
against Codex. All 96 model responses across the retained comparisons passed
the workload checks. No Astra calls were used. Exploratory calls were mostly
Luna; four short Sol calls checked model-specific behavior.

No live latency improvement was measured; these small samples are sensitive
to provider load and are not a statistical speed claim. The existing local
`bench_ttft.py` ask-stdin fixture, ten iterations at 50 ms provider delay,
measured medians of 269.6 ms baseline and 260.5 ms updated. Its overlapping
ranges support only the conclusion that no local slowdown was observed.

## Verification and limits

`make quality`, `make test` (451 unit tests and 42 integration groups), and
`make leaks` passed; the leak checker reported zero leaked bytes. The test
run clears the inherited `TNY_TOOLS=terminal` preference for the child process
so fixtures can exercise their expected tools. The 14 focused cache and
benchmark checks passed. The stripped macOS arm64 binary is 883,904 bytes,
up from 867,312 bytes (1.9%), within the platform budget.

The fixture suite covers stable request prefixes, resume, ephemeral and
isolated runners, retries, the first-token rule, same-process turn reset,
oversized headers, compatible gateways, both OpenAI wires, incomplete and
non-stream responses, duplicate terminal events, nullable cache counts,
weighted benchmark arithmetic, mandatory live opt-in, and authentication of
the local benchmark forwarder. The wasm CI job
runs the shared fixture suite; its native ACP-pipe and fork tests skip.

Wasm uses the same C logic. Browser CORS must allow the routing request
headers and expose the affinity response header; otherwise only the
available routing hints apply. The local measurement is macOS HTTP/SSE:
Codex WebSockets, public API-key billing, browser CORS, and Linux binary
size are not established by it. Cache savings do not directly measure
ChatGPT subscription quota or dollar savings, and cache writes can have
different pricing from uncached input on the public API.

Emscripten and Nix were unavailable locally, so their gates are delegated
to CI. `make quality` explicitly skipped Linux-only GCC `-fanalyzer` on
macOS. No commit, push, or workstation installation is part of this change.
