# ADR 0208: Truthful scoped file search

Status: accepted

## Goal

Find relevant project evidence without silently treating an incomplete search as
an exhaustive absence. Keep the existing portable native tools, not another
indexer or external executable dependency.

## Contract

`grep_files`, `glob_files`, and `semantic_search` share traversal and scope.
Existing result rows remain text; a first-line `[search {...}]` JSON record gives
coverage before any result-size bounding. `files` counts selected regular-file
candidates; `scanned` counts content-read attempts (zero for glob). `excluded`
counts intentionally excluded directory entries, not their descendants. `errors`
counts traversal/read failures; `skipped` counts oversized, binary or invalid
UTF-8 files. `line_truncations`, `file_limit`, and `hit_limit` make bounded loss
explicit. `partial` is true for errors, skipped content, or any such limit, but
not for intentional scope exclusions. Zero matches with partial coverage is
explicitly only zero in the searched portion.

Scope is selected with `path` (default workspace), optional `include`/`exclude`
relative path globs (`*` crosses `/`, `?` matches one character), and booleans
`include_hidden` (default true) and `include_generated` (default false). Hidden
project configuration is discoverable without a vendor-specific allowlist.
Recursive walks omit `.git`, `.hg`, `.svn`, `.tny`, `.env`, `.env.*`, and by
default directories named `node_modules`, `build`, `target`, `dist`, and
`__pycache__`, `.venv`, and `.cache`. Explicit roots are honored: a direct `.env`
file can be grepped, and a `build` root can be searched. These are convenience filters, not secret or
permission boundaries. `include_generated` restores generated directories under
recursive roots. This does not implement `.gitignore`. No recursive symlinks are
followed; existing explicit path resolution and tool permission policy remain.

Traversal stops conservatively at 20,000 selected files, 100,000 entries, or 128
nested directories and reports `file_limit`. Scope filters avoid content reads
but do not promise to prune all unrelated directory enumeration. Grep reads at
most 2 MiB plus one detecting byte per file, rather than loading unbounded files
before rejecting them. It returns at most 500 matching lines, each with a 300-byte
UTF-8-safe preview and an explicit `read_file` hint when cut. Glob returns at most
1,000 names. Lexical search explicitly ranks the best ten files; it is not a
semantic embedding index. Directory visitation order is filesystem order, not a
stable pagination cursor or a consistent filesystem snapshot.

Independent grep reads remain parallel and results fold in traversal order. A
sequentially consistent maximum completed index is published before its hit
contribution. Once completed matches fill the budget, only indices beyond every
contributor can be skipped. An earlier claimed-but-paused file must still scan.
Skipped later indices explicitly make coverage partial. Content-read counts can
vary with scheduling; result rows remain equivalent to serial traversal.
Allocation failures retain the existing OOM
terminal contract. There is no new persistent state, ABI or provider-wire loop.
Wasm retains the existing explicit unsupported execution-server behavior; the
shared file-tool code has no new native platform dependency.

## Verification and measurement

`tests/integration/test_search_coverage.py` exercises real nested tools through
synthetic providers, including both HTTP wires, hidden configuration, explicit
scope, caps, omitted content and symlink boundaries. The old binary fails the
new contract, rather than merely passing a new test that does not distinguish it.
`tests/bench/bench_search_coverage.py BASELINE CANDIDATE` measures hidden-target
retrieval, distracting rows, selected/read counts and model-visible result bytes.
These are not token counts, total task latency or live-agent quality claims.

Measured fixture ([source-linked results](../verification/search-coverage.json)):
on both Responses and Chat Completions, the hidden workflow changes from missed
to found. An explicit `include` scope removes 100 irrelevant matching rows,
reducing model-visible result bytes from 24,928 to 173, with one selected/read
candidate. The baseline has no scan-count instrumentation, so no relative scan
or latency claim is made. Seven production-fixture tests fail on the baseline
and pass on the candidate. No new test directory, external tool, or Nix fileset
is required.

The earlier-index regression uses the actual internal scheduling predicate.
Extracting that function and the test assertions into a standalone C oracle
passes; replacing its condition with the old `hits >= limit` aborts at the first
earlier-index assertion. Existing serial/parallel tests still compare exact
result rows and the 500-hit fold. This negative control does not claim to exhaust
all thread schedules.
