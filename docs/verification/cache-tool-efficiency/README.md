# Cache and tool efficiency iteration

Baseline: `f07acac6` (main after #202). No dependency on verification-cleanup #203.
See [ADR 0190](../../adr/0190-cache-stable-context-and-bounded-discovery.md).

## Measured value

The [local production-wire benchmark](prefix.json) ran five real file-edit and
recovery tasks through each freshly built binary and a synthetic local provider.
After two warmup tasks, the same eligible learning policy stayed active:

| Metric | Baseline | Candidate |
| --- | ---: | ---: |
| Instruction bytes | 5,062 | 5,017 |
| Common instruction prefix across three eligible tasks | 4,810 | 5,017 |
| Identical eligible instruction prompts | no | yes |
| Successful tasks | 5 | 5 |
| Provider requests per task | 5,5,4,4,4 | 5,5,4,4,4 |
| Tool calls per task | 4,4,3,3,3 | 4,4,3,3,3 |

This removes avoidable cache invalidation, not model work. UTF-8 prefix bytes
are not tokens, paid cache hit rates or proven latency/cost savings. Workspace
path lengths contribute to prompt size; each arm measures reuse within its own
workspace. The fixture's task policy is scripted, not inferred by a model.

Reproduce without any paid inference:

```sh
python3 tests/bench/bench_prefix_stability.py \
  --baseline /path/to/base/build/tny --candidate build/tny \
  --output /tmp/prefix.json
```

Both builds used the same pinned prebuilt CPython runtime and compiler. The
benchmark records executable hashes. Existing learning thresholds, evidence
storage and removal of stale advice are unchanged. Skills sort after root
precedence/deduplication; same-root duplicate names remain an existing ambiguity.

The Linux sparse-result regression also enforces a 64 MiB additional address-space
budget: a four-byte slice must work while a whole-file allocation cannot.

Result paging evidence is recorded separately in [result slices](result-slices.md).
MCP discovery now follows the existing preview-plus-spill-handle contract,
including ephemeral sessions. The notice adds bytes beyond the preview; this
is not a claim that the complete string is at most the configured preview cap.

## Verification

- Skills: 9 tests / 84 assertions; learning: 13 / 163.
- Session result checks: 2 / 31; MCP search: 2 / 43; ephemeral: 4 / 26.
- Existing real-code-cell learning fixture covers both native HTTP wires and
  all three profiles. Its new stable-prefix assertion passes the candidate and
  fails the unchanged baseline in all four cases. It adds no extra provider turns.
- Existing prompt-cache routing/usage fixtures: 17 passed.
- Full native unit invocation: 650/666 passed, with 16 runner/ACP socket-related
  failures under the execution sandbox; an isolated `socket(AF_UNIX)` probe
  independently returned `EPERM`. An attempted local-socket escalation
  failed before execution because the environment wrapper could not create its
  session path on a read-only filesystem. Hosted CI is required for that gate.
- ASan/UBSan remained enabled. Local leak detection was disabled explicitly
  because LeakSanitizer aborts under this environment's tracing; no local leak
  pass is claimed.

`make quality` passed, including complete C/C++ clang-tidy, strict warnings
and GCC analyzer plus repository format/script checks. Hosted results must
still be reconciled at the exact published head before calling this iteration
ready to merge. No credentials,
live inference, new provider fields, release or merge are part of this change.
