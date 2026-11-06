# Interaction latency and code-mode experiments — 2026-11-05

Base: `0a40436d4091800ccae795c2d39c99f57758cad4` (v0.37.1), macOS arm64.
Unchanged base built in `../tny-baseline`; candidate branch
`perf/interaction-code-mode`. Both use the same Apple compiler, release flags
and hash-pinned CPython 3.14.7 archive. Candidate shares the baseline's immutable
CPython build via a build-directory symlink; no source dependency is added.

## Retained changes

- Drain buffered HTTP/TLS response data promptly after each fair 8 KiB batch.
- Honor backend scheduling deadlines in external runner/TUI/optimisation loops.
- Tell the agent to batch independent work in one code cell and save reusable
  shell/Python/JavaScript scripts under `~/.tny/scripts/`.
- Keep reproducible transport, code-mode and parallel-read experiments.

No provider/model/effort defaults or permission/network defaults changed.
No credential or private workspace content appears in results. The follow-up
[tool-task evaluation](tool-evaluation.md) retains synthetic generated programs
and capability traces for correctness and token-efficiency review.

## Local harness measurements

Each case runs an actual release binary against a persistent HTTP/1.1 SSE
fixture with synthetic credentials and a fresh home/workspace. The fixture
sends `run_code`, checks its printed result, and completes the next response.
Large calls include a 5,000-character comment; duplicate Responses item events
make the stream exceed the reader's dispatch budget. Latencies are medians of
10 runs, in milliseconds; raw samples are in [baseline.json](baseline.json) and
[candidate.json](candidate.json).

| Path | Metric | Base | Candidate |
| --- | --- | ---: | ---: |
| In process, small call | launch → provider request | 13.9 | 14.3 |
| In process, small call | tool response → continuation request | 24.2 | 25.9 |
| In process, large call | tool response → continuation request | 231.8 | 23.8 |
| Detached runner, small call | launch → provider request | 20.1 | 21.1 |
| Detached runner, small call | tool response → continuation request | 24.1 | 23.6 |
| Detached runner, large call | tool response → continuation request | 136.8 | 23.9 |

The clear improvement is removal of a 100/200 ms polling delay. Start-of-turn
model latency is not reduced by this fix. These fixtures deliberately separate
harness scheduling from inference/network variance. The original optional CLI tool-start field is null because `--json` returns
a final summary rather than streaming events. Request/continuation/total clocks
are server/process timings and unaffected. That unused observer was removed;
this benchmark reports server/process timings only.

## Parallel calls

The existing `tools.parallel` already overlaps independent native reads (up to
four workers), retaining ordered results and serial barriers for stateful tools.
Four 100 ms local `web_fetch` operations measured **454.1 ms serial versus
137.1 ms parallel** through the real cell/execution-server path, five paired
runs. Every output was checked in input order. See [parallel.json](parallel.json).
This validates the existing capability, not a newly implemented scheduler.
Unrestricted concurrent effectful cells are not enabled: independent host
subprocesses can already be launched together within one Python cell.

## Code-mode comparison

Each arm computes the same integer sum of squares, with its output checked,
through the real `run_code` and fresh execution-server boundary. Five runs per
arm; no external provider. [code-modes.json](code-modes.json) contains samples.

| Execution | Median tool continuation (ms) |
| --- | ---: |
| Embedded CPython | 21.7 |
| Host shell (`sh` + `awk`) | 35.9 |
| Host Python | 45.1 |
| Host JavaScript (Node) | 66.9 |
| Monty 1.4.0, fresh external pool | 92.8 |

These host/Monty arms use the existing Python subprocess bridge; they do not
claim to measure a hypothetical direct C→shell or persistent C→Monty adapter.
No default replacement is justified by these production-path results.
A separate warm Monty pool checkout + compute median is about 0.48 ms (30
samples, including its first checkout). Its process startup, host RPC,
permissions, cancellation and output retention are outside this lower bound.
Monty has no ambient host filesystem/environment/network; enabling arbitrary
network operations requires explicitly supplied host functions. Such functions
can follow YOLO policy, but passing every host capability through them is a
larger design than swapping an interpreter. Host CPython already provides the
requested ordinary shell/Python/JS execution and network access.

## Live Sign in with ChatGPT transport probe

Used the user's saved SIWC credentials, refreshed through tny's normal locked
store, with explicit authorization. Model `gpt-6.1-sol`, low reasoning,
`store:false`. Three alternating SSE/WebSocket pairs, three short turns each;
full assistant output items replayed over HTTP and same-connection
`previous_response_id` over WebSocket. Both arms reported matching input-token
sequences (11, 27, 43). [live-transport.json](live-transport.json) stores only
latencies, usage counts and transport names.

| Metric | Persistent SSE | WebSocket |
| --- | ---: | ---: |
| Warm request → first text, median of 6 | 2637 ms | 1472 ms |
| First connect + request → first text, median of 3 | 2648 ms | 3483 ms |

This is a direct Python transport experiment, not a native tny before/after
result. The sample is small, prompts are short, model/server variance remains,
and reasoning-token content is not controlled. WS is promising for later
turns, but the initial handshake cost matters. Keep the probe for a larger
long-context/tool-loop study; native WS is not shipped from this evidence.
The earlier exploratory run omitted assistant items from HTTP continuation and
was discarded. The retained run fixes history equivalence. Its request-byte
metric was removed because the mutable history changed before that metric was
sampled; the probe now snapshots request size before replay history mutation.

## Live native harness before/after

Three alternating baseline/candidate pairs ran actual tny with SIWC,
`gpt-6.1-sol`, low effort, ephemeral sessions and a fresh empty workspace. Each
turn was asked to execute `print(42)` once and return OK. All six turns exited
successfully and emitted tool-start/tool-end events without error events.
Canonical event timings (no content) are in [live-harness.json](live-harness.json).

| Metric, median of 3 | Base | Candidate |
| --- | ---: | ---: |
| Launch → first tool event | 3827 ms | 3594 ms |
| Launch → first text | 6735 ms | 6785 ms |
| Launch → turn end | 6870 ms | 6978 ms |

There is **no demonstrated end-to-end improvement** on this small-frame task.
Provider/model variation dominates. CLI tool events can be delivered together
after synchronous execution, so their timestamp difference is not an execution
time measurement; the local fixture's server-to-server clock measures that
boundary. An initial observer used `--json` (one final summary) instead of
`--events=jsonl`; those observations were discarded and the retained pairs use
the canonical event stream.

## Provider and interpreter documentation reviewed

- [OpenAI Responses WebSocket mode](https://developers.openai.com/api/docs/guides/websocket-mode): persistent connections and incremental continuation;
  reconnects require explicit recovery and connection-local state management.
- [SIWC preview limitations](https://developers.openai.com/siwc/token-sharing-open-source/preview-limitations): HTTP history replay without `previous_response_id`;
  WebSocket continuation limited to the same authenticated connection.
- [SIWC models and inference](https://developers.openai.com/siwc/token-sharing-open-source/models-and-inference): public Responses bearer authentication and streaming.
- [xAI WebSocket mode](https://docs.x.ai/developers/advanced-api-usage/websocket-mode):
  the public Responses API also offers connection-local continuation. This is
  documentation research only; no Grok credential or subscription endpoint was tested.
- [OpenRouter prompt caching](https://openrouter.ai/docs/guides/best-practices/prompt-caching):
  stable prefixes and session routing matter to cache reuse. Provider-specific
  routing changes need their own benchmarks; none are enabled by this patch.
- [Monty documentation](https://pydantic.dev/docs/monty/): sandbox/host-function and pooled worker model. Installed experimental version: 1.4.0.

The existing tny HTTP backend already reuses connections, sets TCP_NODELAY and
uses stable prompt-cache routing. This change preserves those features and the
SIWC/Codex credential boundary. No undocumented ChatGPT backend was used.

## Reproduction

From a buildable checkout, build the unchanged base in a separate worktree and
the candidate with the same toolchain. The ordinary build may download
hash-pinned CPython dependencies. Then:

```sh
python3 tests/bench/interaction/bench.py --tny ../tny-baseline/build/tny --runs 10 --output /tmp/base.json
python3 tests/bench/interaction/bench.py --tny build/tny --runs 10 --output /tmp/candidate.json
python3 tests/bench/interaction/parallel.py --tny build/tny --runs 5 --output /tmp/parallel.json
python3 -m venv /tmp/tny-experiments
/tmp/tny-experiments/bin/pip install pydantic-monty==1.4.0 websocket-client==1.9.2
python3 tests/bench/interaction/code_modes.py --tny build/tny --monty-python /tmp/tny-experiments/bin/python --runs 5 --output /tmp/code-modes.json
python3 tests/bench/bench_ttft.py --tny build/tny --repo . --bench ask-stdin --iters 5 --rpc-delay 0
# Live requests consume the signed-in user's plan; explicitly opt in:
build/tny --provider openai status
/tmp/tny-experiments/bin/python tests/bench/interaction/live_transport.py --live --pairs 3 --output /tmp/transports.json
python3 tests/bench/interaction/live_harness.py --live --baseline ../tny-baseline/build/tny --candidate build/tny --output /tmp/live-harness.json
```

## Verification

Initial `make test` hit missing
Z3 in PATH and an existing macOS `/var` versus `/private/var` temporary-path
assertion. Retried with the installed Z3 and `TMPDIR=/private/tmp`.

Release sizes at measurement: base 25,328,224 B, candidate 25,328,240 B.
Direct dependencies: libSystem and libc++; TLS remains loaded at runtime.

- Full candidate unit run: 742 passed, one skipped, one failed. The failure is
  `terminal_multiple_tasks_do_not_steal_other_children_or_fds` at line 149
  (expected exit 4). It passes when run alone, but fails in full-suite order.
- Unchanged baseline full unit run reproduces that exact failure (742 passed,
  one failed, one skipped). It is a pre-existing blocker to a green `make test`,
  not evidence of a regression from this patch. Full integration does not run
  after that make prerequisite fails; affected integration files run separately.
- Focused OpenAI/error-policy suites: 43 tests passed, including the new
  buffered-body scheduling/EAGAIN regression, under ASan/UBSan.

- All eight affected integration files passed: OpenAI, SIWC, execution code mode
  (43 tests, one platform skip), host code mode, terminal cancellation, TUI,
  optimisation (19 tests), and CLI/native-library event conformance.
- `make quality` passed with pinned clang-format 23.1.0, clang-tidy 22.1.8,
  Ruff 0.16.6 and the repository's other pinned linters. Darwin's expected
  GCC-analyzer skip is reported by the gate; Linux/wasm CI was not run here.

- Existing `bench_ttft.py --bench ask-stdin --rpc-delay 0 --iters 5`:
  baseline median 87.3 ms (81–93 ms), candidate 88.8 ms (85–93 ms). This
  confirms no material startup benefit in that existing small-frame scenario.
- macOS `make leaks` completed with exit 2: affected OpenAI/runtime/TUI/net
  suites and CLI smoke checks report zero leaks. `task_workspace_suite` failed
  five assertions under the checker and reported 7 leaks / 6,640 bytes after
  those failed test cleanups. This gate is not reported as passing.

- The unchanged baseline's sanitizer-free `task_workspace_suite` reproduces
  the same five assertion failures and 7 leaks / 6,640 bytes under
  `leaks --atExit`; the leak gate therefore has a second baseline blocker.
  No claim is made that failing-test cleanup leaks are harmless in general.

## PR #233 macOS fixture correction

The first CI run failed the new buffered-body test in the parser backend
ownership sanitizer target. This was reproduced locally: 42 passed, one failed,
one skipped. The fixture assumed a successful peer write made the entire batch
immediately readable by the client; an early EAGAIN legitimately returned a
normal timeout before the dispatch exhausted its budget.

The corrected fixture waits for the specified receive-queue byte count without
consuming parser input. It deliberately delivers headers before the body and
checks normal waiting, then delivers the full body batch and checks immediate
rescheduling. It additionally checks that the socket receive queue is empty
while HTTP still holds the unfinished event, verifies all 12,000 text bytes
are delivered on the next dispatch, and checks normal waiting again before
sending the terminal event. No production scheduling logic changed.

Validation: the exact `make test-parser-backend-ownership` target passed
(43 passed, one expected skip), the normal OpenAI sanitizer suite passed
(32/32), and the corrected test passed 100 consecutive ownership-sanitizer runs.
Two temporary production mutants were both rejected: suppressing immediate
rescheduling fails the zero-timeout assertion; retaining the yield flag across
EAGAIN fails the normal-wait assertion. This preserves both latency and
no-busy-loop coverage. Formatting and diff checks passed. See
[fixture-validation.json](fixture-validation.json).
