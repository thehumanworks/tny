# ADR 0212: Size-aware content returns

Status: accepted. Supersedes the unrestricted-read spill behavior in ADR 0211;
its bounded line scanning and formatting contract otherwise remain unchanged.

## Goal

Keep bulk file/page content out of the agent's context until it selects useful
sections, without losing access or fetching the same page again.

## Decision

Reuse `max_tool_result_bytes` (default 32,768 bytes), not a second content budget
or token estimator. Global settings, workspace settings, then repository
`.tny.json` may set an integer from 1 through 16 MiB; later layers override earlier
ones. Invalid values fail configuration rather than overflowing into an unlimited
budget. This is the existing shared tool-result limit, not a web-only setting.

Local `read_file` opens the file once. An oversized regular file requested without
a positive line limit returns its original resolved path and file size after
image sniffing, without copying the file into session storage. All local reads
also bound actual accumulated output, so growth after `fstat`, nonregular files,
and giant selected lines cannot bypass the content budget. An oversized selected
range returns guidance to narrow it. Explicit useful line ranges retain the
historical newline formatting; a final synthesized newline counts toward the
budget. Searching with `grep_files`/`rg` and then reading a few lines is the normal
recovery. For a single huge line, use bounded Python seek/read or `head -c`.
There is no claim that arbitrary terminal/Python reads are intercepted.

`web_fetch` and configured `web_search_url` responses above the limit retain
original response bytes through the existing bounded, atomic session-result
writer. The result reports HTTP status, source URL, original Content-Type, byte
size, actual session-owned `.txt` storage path, and `read_tool_result` handle.
The neutral extension denotes a stored byte stream, not converted Markdown:
Markdown remains Markdown and HTML remains HTML. No renderer/converter or second
cache is introduced. Small bodies retain the original output format.
DuckDuckGo's internal result-page parsing and Codex search summaries are not
misrepresented as downloaded destination pages. Shell search overrides retain
the terminal tool's existing behavior.

Page responses stream into the existing writer after crossing the inline limit,
up to its 64 MiB disk bound rather than the old 1 MiB fetch cutoff. Larger,
interrupted or timed-out responses are explicitly marked INCOMPLETE with retained
bytes and unknown total; the artifact is only the retained prefix. Internal
DuckDuckGo parsing retains its 1 MiB limit. Exact-boundary complete responses are
not marked incomplete. Storage
failure is explicit and does not refetch the URL. Downloaded content is data,
never executed by this feature.

## Ownership and platforms

Saved content uses the existing random-ID session result path, private directory
and 0600 atomic artifact writer, with sync/close/rename checks and temporary-file
cleanup. It survives execution-worker exit and session resume, and follows the
existing session deletion lifecycle. It is not a separate permanent download
cache. Path-based reads continue to obey ordinary permissions; the scoped result
handle works without granting broader filesystem access.

`--no-save`/ephemeral retains at most the existing aggregate 1 MiB memory budget
and returns no fictional filesystem path. It expires when the session closes.
Ephemeral web fetches run serially through the existing owner execution seam so
parallel workers cannot overcommit that shared budget; saved-session web fetches
remain parallel. Exhaustion leaves existing handles readable and reports that
new content could not be retained.

Under SSH, web requests and saved artifacts remain on the local controller.
The response labels that fact and directs recovery through `read_tool_result`,
not remote `read_file`. The SSH file reader remains its existing separately bounded
remote transport; the new early local-file guard does not claim remote I/O
savings. Wasm shares the native tool implementation over its virtual filesystem
when directly embedded; provider-facing `run_code` still returns the documented
unsupported-execution error. No host-visible path is invented for memory-only
results.

## Verification

- Existing range compatibility/fault fixtures plus threshold−1/equal/+1,
  unrestricted/offset-only/giant-selected-line guards, image precedence and
  unchanged source/no duplicate artifacts on both provider wires.
- Local HTTP fixtures: small/threshold bodies, original HTML/Markdown byte
  recovery, one fetch across recovery, saved permissions and fresh-cell handles,
  URL-override search, and parallel ephemeral budget exhaustion.
- Config precedence and invalid values at all three layers.
- Before/after emitted-byte measurements and full gates recorded with the PR.

These checks prove local contracts and payload reduction, not live-agent task
quality, provider token counts, or inference latency.

### Measured payload fixture

`tests/bench/bench_content_bounds.py` compares the unchanged base `2bb01484`
against this change using the same local mock, 320,012-byte source, saved session
and compiler. Both Responses and Chat produced identical counts:

| Tool | Before | After | Reduction | HTTP fetches |
| --- | ---: | ---: | ---: | ---: |
| `read_file` | 16,538 bytes | 332 bytes | 98.0% | 0 → 0 |
| `web_fetch` | 16,538 bytes | 461 bytes | 97.2% | 1 → 1 |

Counts are actual model-visible tool-result UTF-8 bytes, including notices, not
token estimates. Random fixture path lengths are fixed. The eight web integration
methods additionally prove full recovery of original HTML/Markdown, fresh-cell
handles, content beyond the former 1 MiB cutoff, incomplete-response provenance,
private artifacts, explicit storage failures and bounded ephemeral retention.
The file range suite proves that choosing a useful section returns its original
contents without creating a copied-file artifact.

## Standalone fetch ownership

`tny web fetch` has no conversation session. On spill it creates a private
absolute-path temporary directory through a sessionless owner mode of the same
result-stream writer, preserving its byte cap, atomic rename and cleanup. It
returns the real file path, never a session handle; the file remains readable
after native command exit until manual or OS temp cleanup. No conversation
metadata is created. Wasm paths remain virtual-filesystem paths, not durable
host files; SSH does not relocate controller-local content. Standalone no-save
requests reject oversized responses before creating storage because a memory
handle would die at command exit. Small outputs remain unchanged.

CLI fixtures verify exact post-exit bytes, one fetch, private permissions,
manual cleanup, no-save rejection and an unusable temporary root. A stream
abort regression verifies removal of both staged file and private directory.
