# ADR 0171: Experimental line-aware tool result spilling

Status: proposed

## Decision

`TNY_EXP_SPILL=1` enables line-aware previews of large tool results. Output
larger than `TNY_EXP_SPILL_BYTES` (8192 by default) is stored in the session's
result directory, using a SHA-256 content handle. The inline result carries
head and tail lines, with one quarter of the budget reserved for the head by
default, and the remainder for the tail. `TNY_EXP_SPILL_HEAD_PCT` tunes that
share (0–100); `TNY_EXP_SPILL_LINE_BYTES` caps each line's displayed source
bytes (1024 by default). A clipped head line shows its start and a clipped
tail line shows its end; each has an omitted-byte marker. A header reports total bytes,
lines, shown line ranges and the full path. The content digest keeps repeated
output at one stable path without using output text in the file name. Existing
`read_tool_result` handles continue to work. Ephemeral sessions store results
in memory. Spill failure falls back to the prior head-only preview.

Under the same flag, `read_file` uses `TNY_EXP_READ_BYTES` (16384 by default)
for whole lines, reports the next line offset, and optionally marks every tenth
line when `TNY_EXP_READ_LINENO=10`. A line too long to fit is reported without
advancing the offset, and its full file receives a byte-range handle when a
session is available. Terminal collection in the `all` profile uses the same
64 MiB hard cap as shell profiles so outputs beyond 512 KiB can be spilled.

The flag is captured in `tny_ctx` and carried into detached session runners
through private start-packet fields. Those fields are omitted from flag-off
packets and public recovery snapshots. Without the flag, provider request
bytes and tool behavior remain unchanged.
The shared file and result code works on native and wasm; wasm's terminal tool
continues to return its existing unsupported error.

## Measurement

The local mock Responses replay used a 40,000-line terminal log, a 5,000-line
failing test output, and a 12,000-line `read_file`. Each case made two provider
requests. The first request carried no tool result; the second sizes were:

| Case | Inline bytes off/on | Second request bytes off/on |
| --- | ---: | ---: |
| Terminal log | 16,537 / 8,442 | 40,233 / 31,832 |
| Failing test | 16,536 / 8,444 | 40,499 / 31,973 |
| `read_file` | 16,537 / 16,531 | 40,137 / 40,127 |

The first request ranged from 22,905 to 22,912 bytes in these runs. The file
read is primarily a navigation change at its default 16 KiB budget. These are
synthetic tool outputs and do not establish a live inference quality gain.
The replay used native default isolation (`TNY_ISOLATE` absent), so the tool
ran in a detached session runner. The second request contained one spill
header in each flag-on terminal case and one continuation header in the
flag-on `read_file` case; flag-off cases contained neither. The three flag-off
request-size pairs exactly matched an isolated build of `main` at
`41a3b82861b057c85ce86339292fdddd8846c76d`. This checks the complete
caller-to-runner path and the flag-off wire boundary.
With `TNY_EXP_SPILL_BYTES=16384`, the same replay put 16,651 bytes of terminal
log output and 16,633 bytes of failing-test output inline; the second requests
were 40,345 and 40,593 bytes respectively. The 16 KiB arm slightly exceeds
the flag-off request sizes because its path and line-count header is longer.

## Rollback

Unset `TNY_EXP_SPILL` to restore the previous wire and tool-result behavior.
The added result files are removed with their sessions.
