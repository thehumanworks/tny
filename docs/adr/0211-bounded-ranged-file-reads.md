# ADR 0211: Bounded ranged file reads

Status: accepted

## Goal

Reading a small line range must not allocate or read the whole local file.

## Decision

Local `read_file` scans fixed 8 KiB chunks, discards skipped lines, and stops
when the requested positive line limit is satisfied. Memory is proportional to
selected output plus scanning overhead. A selected huge line still needs its
output storage; later offsets still scan the preceding bytes. No index, cache,
new tool, or alternate filesystem authorization path is introduced.

Preserve the existing range format, including a newline after an unterminated
final line and the empty final line after a trailing newline. Nonpositive offsets
start at line one; nonpositive limits remain unlimited. Calls with neither a
positive offset nor a positive limit keep the whole-file implementation.
The first chunk retains existing image detection. Unicode, CRLF and NUL bytes
are not newly decoded or normalized; existing downstream text limitations remain.
Output bounding and durable result recovery remain unchanged.

A range is not validation of the whole file: read failures in an unread tail are
not observed after the requested range is complete. Errors encountered while
reading the required prefix fail the call rather than returning partial success.
Stdio may read ahead within its fixed buffer. SSH-backed reads are unchanged.

## Verification

Both synthetic provider wires exercise the production tool for exact range
formatting, EOF, empty files, huge lines, Unicode, NUL, image rejection and path
failures. The read-owner benchmark compares identical compiler flags and reports
bytes returned by `fread`, fresh-process peak RSS and elapsed time separately;
these are not physical disk-I/O or end-to-end model latency measurements.
EOF and late offsets intentionally retain prefix-scan cost.

On Linux with identical `-Os` owner builds, three fresh-process samples per size
read the first 20 lines of 1/100/1000 MiB sparse files. Median bytes returned by
`fread` fell from the full file size to 8 KiB at every size. Median owner latency
fell from 0.508/54.480/486.349 ms to 0.023/0.032/0.030 ms; peak RSS at 1000 MiB
fell from 1,024,640 KiB to the 12,032 KiB process floor. All output hashes matched.
Sparse tails and uncontrolled page cache mean this is not a cold-disk benchmark.
[Raw samples](../verification/read-file-ranges.json) record compiler and source
hashes. Reproduce with `python3 tests/bench/bench_read_file_ranges.py --output
/tmp/read-ranges.json` from the repository root.
