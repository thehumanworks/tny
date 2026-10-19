# Job lifecycle verification cost

The durable-job suite checks real workers, locks, process trees, persistence,
credentials and local provider traffic. Those assertions are retained; this
change removes repeated setup work, not production safeguards.

## Scope and coverage

- A helper-only `DAGFixture` replaces inheritance from the concrete `JobsDAG`
  test case. Seven canonical scenarios previously ran in each of five classes.
  All 122 original distinct test methods remain; 28 inherited reruns are removed.
- Enrollment, review and soft-budget subclasses did not customize setup. Named
  provider selection did add settings and a secondary server, so one explicit
  default-provider isolation test preserves that distinct interaction.
- Generic failed-job setup now returns HTTP 400, not HTTP 500. These tests need
  a failed item before testing retry transactions, stale snapshots, ownership,
  dependency blocking or credential isolation. They do not need to repeatedly
  wait through the same production 1/2/4-second transport backoff.
- One explicit integration test compares permanent rejection with exhausted
  transient rejection on both Chat Completions and legacy Responses wires. It
  verifies one versus four provider requests, attempt identity, failed job and
  item states, and the child's failure exit code. It clears inherited
  `TNY_PROVIDER_RETRIES` so the default production budget is actually tested.
- Both fixture HTTP servers use a 50ms idle poll interval instead of the
  Python default 500ms. This only removes repeated shutdown latency; it does
  not reduce a test deadline, request hold, retry delay or failure budget.
- `test_openai.py` continues to cover transient recovery, Retry-After, retry
  budget overrides, permanent rejection and streaming retry behavior. Image
  failures, concurrency holds, cancellation deadlines, process ownership and
  race barriers are unchanged. No production code or CI gate changes.

The resulting suite discovers 124 tests, with no inherited duplicate test
methods. On native Linux the two wasm-only tests are explicitly skipped, as
before. More tests or more elapsed time are not evidence of additional coverage.

## Reproduction and evidence

Run the original and revised `tests/integration/test_jobs.py` with the same
release binary using `TNY=/absolute/path/to/tny python3 ...`. Both runs must use
throwaway fixture HOME directories and local synthetic credentials. Record the
Python unittest summary, and distinguish fixture runtime from overall CI time.

Local Linux x86_64 / Python 3.12.14 measurements used one unchanged release
binary from main `f07acac68969675995764f6675c4803fb90ff6b0` (SHA-256
`0bc18a5108a84127a028405915596572da04beac3db900302881058f81b7304e`).
These are single fixture-suite runs on a shared host, not controlled platform
benchmarks; compilation and other fixture runs overlapped. No hosted pipeline
speedup is claimed until exact-head CI finishes.

Two deliberately broken production builds were tested, then restored:

| Negative control | Required observation | Result |
| --- | --- | --- |
| Aggregate a failed item as a succeeded job in `jobs.cpp` | Durable state assertion fails on both wires and both statuses | Killed: four subcase failures |
| Change `OA_MAX_RETRIES` from 3 to 0 | HTTP 500 request-count assertions fail on both wires | Killed: two failures, actual 1 versus expected 4 |

The restored release binary passed the new four-subcase test in 15.107s.
The full fixture comparison below uses the same unchanged baseline binary,
not either mutant. AST comparison found no removed original test methods and
no changed original test-method bodies. The independent review checked the
fixture variant and retry coverage, not just the reduced test count.


| Suite configuration | Discovered tests | Elapsed | Outcome |
| --- | ---: | ---: | --- |
| Unchanged main fixture | 150 | 512.320s | Pass, 2 wasm-only skips |
| Helper-only DAG + explicit failure semantics | 124 | 161.575s | Pass, 2 wasm-only skips |
| Final, also responsive fixture shutdown | 124 | 120.613s | Pass, 2 wasm-only skips |

The final fixture suite saved 391.707s (76.5%) in this local comparison.
This is not a claim that the complete CI pipeline is 76.5% faster. The previous
hosted critical path was dominated by the broad native build/test step; exact-head
hosted results must establish how much of this fixture saving carries through.

Representative same-scenario timings before the shutdown adjustment: stale
carried-success rejection 7.522s → 0.518s; failed-DAG input-change checks
8.410s → 1.275s. The new explicit transient-exhaustion integration test still
spends about 15 seconds exercising real backoff rather than hiding it.
