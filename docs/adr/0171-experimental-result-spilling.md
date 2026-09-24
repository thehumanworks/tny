# ADR 0171: Experimental line-aware tool result spilling

Status: proposed

## Decision

`TNY_EXP_SPILL=1` enables line-aware previews of large tool results. Output
larger than `TNY_EXP_SPILL_BYTES` (8192 by default) is stored in the session's
result directory, using a SHA-256 content handle. The inline result carries
head and tail lines, with one quarter of the budget reserved for the head by
default, and the remainder for the tail. `TNY_EXP_SPILL_HEAD_PCT` tunes that
share (0–100); `TNY_EXP_SPILL_LINE_BYTES` caps each line's displayed source
bytes (1024 by default). Each clipped line shows both its start and end with
an omitted-byte marker, including long lines followed by later errors. The
header and separator count toward the inline budget. A header reports total bytes,
lines, shown line ranges, the full path and an explicit `read_tool_result`
handle. The handle is available in `ask`/`auto` modes and under `--ssh`, where
the path is local to the session rather than the remote machine. The content
digest keeps repeated
output at one stable path without using output text in the file name. Existing
`read_tool_result` handles continue to work. Ephemeral sessions store results
in memory without repeated copies of identical output. Spill failure falls
back to the prior head-only preview. NUL bytes in previews are escaped; skill
bodies and sub-agent reports retain their existing whole-text 32 KiB bound.

Under the same flag, `read_file` uses `TNY_EXP_READ_BYTES` (16384 by default)
for whole lines, reports the next line offset, and optionally marks every tenth
line when `TNY_EXP_READ_LINENO=10`. An oversized line returns a UTF-8-safe
slice and a nonnegative `byte_offset` naming the next byte position. Negative
line offsets retain the pre-existing first-line behavior. Remote reads use
the same preview up to their existing 8 MiB file limit. Flag-on terminal
collection in the `all` profile streams the full output to a temporary file,
then stores it under a content handle; this retains the real tail beyond 8 MiB
without holding the collection in memory. Shell profiles retain their
separate 64 MiB cap.

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
| Terminal log | 16,537 / 8,166 | 40,243 / 31,597 |
| Failing test | 16,536 / 8,185 | 40,509 / 31,750 |
| `read_file` | 16,537 / 16,541 | 40,147 / 40,192 |
| 530 KB terminal straddle | 16,537 / 1,418 | 39,724 / 24,646 |
| Long final progress line | 16,536 / 1,416 | 47,904 / 25,141 |
| 40 KB one-line file | 16,536 / 16,551 | 39,633 / 39,689 |
| Long line followed by `npm ERR!` | 20,055 / 1,439 | 43,163 / 24,592 |
| 8.5 MB output with final marker | 16,537 / 8,176 | 41,279 / 32,102 |

The file read is primarily a navigation change at its default 16 KiB budget.
These are synthetic tool outputs and do not establish a live inference quality
gain.
The replay used native default isolation (`TNY_ISOLATE` absent), so the tool
ran in a detached session runner. The second request contained one spill
header in each flag-on terminal case and one continuation header in each
flag-on `read_file` case; flag-off cases contained neither. Against an isolated
build of `main` at `41a3b82861b057c85ce86339292fdddd8846c76d`, the
complete flag-off request bodies match after normalizing only the random
16-character result handle, and the stored result-file contents match byte
for byte. The first request bodies match raw. The straddle case checks the
original 512 KiB chunk overshoot and stores 525,043 identical bytes in both
arms. The progress preview contains its final `ERROR`, the long nonfinal line
contains `END_OF_LONG_LINE`, and the 8.5 MB output contains its real
`REAL_END_MARKER` tail. The one-line read
reports `byte_offset=16384` for byte continuation. This checks the complete
caller-to-runner path and the flag-off wire boundary.
With `TNY_EXP_SPILL_BYTES=16384`, the same replay put 16,352 bytes of terminal
log output and 16,378 bytes of failing-test output inline; the second requests
were 40,090 and 40,378 bytes respectively. Both totals include the header.

## Rollback

Unset `TNY_EXP_SPILL` to restore the previous wire and tool-result behavior.
Result files remain inside the session directory until that directory is
removed, as with existing result handles.
