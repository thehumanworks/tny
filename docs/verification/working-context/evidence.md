# Recoverable working context evidence

Scope: ADR 0192. No live provider credentials or inference used.

## Reproducible local evidence

- Synthetic single-user-turn transcript: 12 tool calls with 8,192-byte results.
  Like-for-like serialized message views: 101,425 bytes before folding and
  26,861 bytes after folding at a 32,768-byte test budget. This is 74,564 fewer
  bytes (73.5%), not a measured token, latency or task-success improvement.
- Five focused unit tests, 94 assertions: exact middle-of-output recovery,
  identical working view after save/reopen, original transcript equality,
  unchanged message counts and latest two batches, protected fields, invalid
  references and overflow offsets. Additional cases cover ephemeral recovery,
  UTF-8/JSON escaping, zero budget, restricted profiles and repaired transcripts
  with a nonzero boundary. ASan/UBSan pass; local LeakSanitizer is disabled
  because this executor's ptrace environment does not support it.
- The local HTTP fixture in `tests/integration/test_prompt_cache.py` drives a
  12-output autonomous turn followed by exact hidden-middle recovery through
  real `run_code` and `read_tool_result`. Both Chat Completions and Responses
  complete 14 requests without credentials. It checks stable instruction
  prefixes, stable previously folded outputs and preservation of the user goal.
  The full file's 18 tests pass.
- A protected-only view intentionally exceeds an impossibly small budget,
  retaining existing missing-result repair and original pending state unchanged.

Run the focused unit cases with `ASAN_OPTIONS=detect_leaks=0
build/tny-test -t session_working_context` and the integration fixture with
`python3 tests/integration/test_prompt_cache.py build/tny`.

## Interpretation

The change reduces repeated old-output payload while preserving recoverable
source evidence. It does not establish real-provider cache-hit rates, production
success, a total context-window cap, a peak-memory bound or a token budget.
Protected text and the newest two batches may exceed the soft budget. Restricted
profiles retain full output because they cannot use recovery. Explicit manual
compaction and previously compacted boundaries retain their legacy semantics.

## Executor limitations

The aggregate local unit run passes 655 of 671 tests. The 16 failures are ACP
stdin/spawn and runner Unix-socket cases; representative failures reproduce with
an unchanged pre-existing test binary. The executor prevents their socket/spawn
operations. An elevated rerun could not launch because the executor's read-only
mount setup failed. These are not reported as passing; hosted CI must verify the
full suite. The standalone formal checks pass with the existing Clang 19 tools.

The aggregate integration run also encounters executor restrictions: ACP
fixtures cannot create their private local MCP bridge sockets. No successful
aggregate integration run is claimed here. The focused provider fixtures above
run against the rebuilt release executable.

`make quality` passes in this executor, including formatting, full C/C++
clang-tidy, strict diagnostics, Ruff/ShellCheck/workflow/structural/JavaScript
checks, and GCC C/C++ analyzers. Hosted exact-head CI remains the authoritative
full-suite portability check.
