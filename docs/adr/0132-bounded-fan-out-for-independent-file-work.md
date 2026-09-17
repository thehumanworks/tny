# ADR 0132: Bounded fan-out for independent per-file work

Date: 2026-09-17. Status: accepted.

## Context

A request asked to parallelize the sequence of `check()` calls in
`core/checkpoint.cpp`'s `encode()`, and any similar sequential code in
`src/core`. Measured on the ADR 0126 fixture (4-vCPU Linux, `-Os` release
objects, best of 7 x 4000):

| Operation | Time |
| --- | --- |
| private `encode()` incl. mutable-document new/free | 1.3 us |
| public encode + identity (SHA-256) | 7.0 us |
| `pthread_create` + `pthread_join`, one no-op thread | 18.3 us |

`encode()` is not a candidate, for three reasons that each suffice: every
`check()` there mutates one `yyjson_mut_doc`, whose value and string pools
are not thread-safe, so the calls form one dependency chain through the
document; the whole function costs less than one thread creation; and the
allocation-failure scope (`tny_alloc_scope_failed`) is thread-local, so an
exhaustion on a worker would be invisible to the caller. It runs once per
session-runner restart. It stays as it is.

A survey of `src/core` found four loops that are genuinely independent per
item and dominated by file reads plus CPU work: the `grep_files` scan, the
`semantic_search` scan (a full lowercase pass and up to eight `strstr`
passes per file), image export source loading (read + SHA-256 + header
parse per source, up to 64) and image edit reference loading (same, up to
5). Each already writes into an index-addressed slot; only the final order
and the error reported are sequential.

## Decision

`src/util/parallel.h` adds `tny_parallel_for(count, fn, ud)`: a fork/join
pool that exists only for the duration of one call. Workers claim indices
from an atomic counter in increasing order, the calling thread is one of
the workers, and every thread is joined before the call returns, so no
thread outlives the call and later `fork()` sites still see a
single-threaded process. Worker count is `min(online CPUs, TNY_THREADS,
8)`. The call runs serially on the calling thread when there are fewer
than two items, when one worker is allowed (`TNY_THREADS=1`), when a thread
cannot be created (the wasm build has no threads and takes this path
unchanged), and while allocation fault injection is armed, so the fault
harnesses keep enumerating every allocation index deterministically.

Allocation failure keeps one oracle: a worker that observes exhaustion in
its own thread-local scope reports it through the new
`tny_alloc_scope_note_failure`, which the join folds into the caller's
scope before `tny_parallel_for` returns.

Items must write only to their own slot. They may not touch `tny_ctx`,
session state, a shared `yyjson_mut_doc` or `buf_t`, and may not fork. The
four call sites follow that rule:

- `grep_files` walks first (the walk is `lstat` only), then scans the
  collected files in rounds of 256 into per-file buffers and folds them in
  walk order up to the existing 500-line cap. A shared hit counter lets a
  file skip its read once the lines already scanned ahead of it fill the
  cap; because indices are claimed in order, that skip is exact, and the
  serial path stops reading at the same point the old scan did.
- `semantic_search` scores every collected file in parallel and ranks the
  scores in walk order, so ties resolve to the earlier file exactly as
  before.
- Image export sources and edit references load into their own slots with
  a per-slot error buffer; the first failing slot in request order is the
  error reported, and cancellation is probed per item through the existing
  atomic flag.

Output bytes, hit caps, ranking ties, error messages and the OOM contract
are unchanged; only wall-clock time changes.

## Measurements

Same machine and objects as above; `tools_execute` timed end to end, best
of 7, output hashes identical across all three columns. The baseline is
the pre-change commit built in a git worktree.

| Workload | Baseline | New, `TNY_THREADS=1` | New, 4 threads |
| --- | --- | --- | --- |
| 1373-file tree (23 MB): grep rare pattern | 48.3 ms | 49.0 ms | 16.9 ms |
| 1373-file tree: grep common pattern (500-line cap) | 4.2 ms | 4.9 ms | 3.9 ms |
| 1373-file tree: grep with no match | 51.3 ms | 58.9 ms | 19.6 ms |
| 1373-file tree: semantic_search | 63.2 ms | 64.3 ms | 19.3 ms |
| 400 x 200 KB tree: grep rare pattern (cap) | 46.1 ms | 50.8 ms | 14.4 ms |
| 400 x 200 KB tree: grep case-insensitive (cap) | 78.7 ms | 82.2 ms | 22.6 ms |
| 400 x 200 KB tree: grep with no match | 209.4 ms | 230.8 ms | 59.4 ms |
| 400 x 200 KB tree: semantic_search | 307.6 ms | 306.7 ms | 78.5 ms |
| export capture, 8 x 6 MiB PNG sources | 271.5 ms | 271.7 ms | 69.5 ms |
| export capture, 2 x 6 MiB PNG sources | 67.6 ms | - | 31.6 ms |

The serial column is within 10% of the baseline; the difference is the
collected path list. Stripped release `tny` grows by 8,192 bytes (one
page: 1,035,600 to 1,043,792), far inside ADR 0121's limit.

## Alternatives and limits

Threading `encode()` or the other single-document serializers would add a
data race and slow them down; they are excluded above. A persistent thread
pool was rejected: it would leave threads alive across the `fork()` sites in
`tools_shell.c`, `subagent.c`, `ssh.c`, `extensions.c` and `jobs.cpp`, which
`mcp.c` already documents as a hazard. Loops whose order carries meaning
(instruction-file precedence, skill discovery, extension hook dispatch, job
reservation claiming) are not converted. Job result analysis under the
transaction lock and carried-success re-verification are larger candidates
that touch the transaction document; they are left for their own change.
This ADR adds no runtime or third-party dependency and no public ABI.

## Verification

`tests/test_util.c` covers index coverage, the serial cases, the
`TNY_THREADS` cap and the worker-failure fold. `tests/test_core.c` builds a
900-file tree and requires byte-identical `grep_files` and
`semantic_search` output between `TNY_THREADS=1` and the default, with the
cap landing mid-walk. Existing image service and export suites cover the
per-slot loaders unchanged.
