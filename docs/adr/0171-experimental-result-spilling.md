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
slice and a negative `offset` naming the next byte position. Remote reads use
the same preview up to their existing 8 MiB file limit. Terminal collection
in the `all` profile has an 8 MiB hard cap under the flag. This bounds the
combined collection, result, digest and ephemeral-copy footprint to tens of
MiB rather than hundreds; shell profiles retain their separate 64 MiB cap.

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
| Terminal log | 16,537 / 8,541 | 40,240 / 31,938 |
| Failing test | 16,536 / 8,543 | 40,506 / 32,079 |
| `read_file` | 16,537 / 16,538 | 40,144 / 40,141 |
| 530 KB terminal straddle | 16,537 / 2,478 | 39,721 / 25,660 |
| Long final progress line | 16,536 / 2,475 | 47,901 / 26,666 |
| 40 KB one-line file | 16,536 / 16,558 | 39,630 / 39,648 |

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
arms. The progress preview contains its final `ERROR`; the one-line read
reports `offset=-16384` for byte continuation. This checks the complete
caller-to-runner path and the flag-off wire boundary.
With `TNY_EXP_SPILL_BYTES=16384`, the same replay put 16,754 bytes of terminal
log output and 16,736 bytes of failing-test output inline; the second requests
were 40,459 and 40,707 bytes respectively. The 16 KiB arm slightly exceeds
the flag-off request sizes because its path, handle and line-count header are
longer.

## Rollback

Unset `TNY_EXP_SPILL` to restore the previous wire and tool-result behavior.
Result files remain inside the session directory until that directory is
removed, as with existing result handles.
