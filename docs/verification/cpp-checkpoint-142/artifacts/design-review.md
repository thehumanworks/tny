Checkpoint restore and recover should stay first, and the file has two defects beyond plain manual cleanup. I implemented nothing, because this session is read-only and in plan mode. Nothing was built or run, so every defect below comes from reading source. The full review is in `/Users/tomas/.claude/plans/read-only-design-review-do-wobbly-hippo.md`.

## Revised ranking

1. **Checkpoint restore and recover, unchanged.** Restore runs on every isolated turn, at `src/core/runner.cpp:1866`.
   - Eight of 27 string copies in `src/core/checkpoint.c` are unchecked, at lines 144, 147, 150, 183, 186, 231, 234 and 237.
   - A NULL auth header name or prefix reaches the `"%s: %s%s"` format at `src/backends/openai/openai.c:1110`. That is undefined behaviour, and on glibc it sends the key under a `(null)` header.
   - A NULL instructions snapshot with the ready flag set makes `src/core/instructions.c:158-160` emit no instructions, with no error.
2. **OpenAI request scope, promoted from third.** The credential wipe-and-free sequence is written three times, at `openai.c:1155-1160`, `1195-1199` and `1237-1242`. It is also the only candidate with an armed fault scope, at `tests/test_openai.c:1189-1250`.
3. **MCP stdio connection, promoted from fourth but gated.**
   - `conn_open_stdio` at `src/mcp/mcp.c:190-274` owns four descriptors and a child process across six exits.
   - `src/mcp/mcp_http.c:50` calls `waitpid` with `WNOHANG` right after `SIGTERM`, so closed stdio servers become zombies. This should be fixed in C now, independent of any conversion.
   - There are no MCP fault scopes, so a new one must exist before the conversion starts.
4. **Session storage, demoted from second.** The cleanup ladders are shallow and mostly correct. The one real defect is an unchecked id copy at `src/core/session.c:1309`, dereferenced at line 1361. Scope the work to the session list and task reconcile only.
5. **Config, last, and better fixed in C.** The context is a plain C record with many C writers, so a C++ owner cannot hold its fields. One checked C setter fixes the free-then-unchecked-copy pattern there and also serves item 1.

## Second checkpoint defect: recovery appends headers

`tny_finish_builtin_profile` calls `tny_ctx_add_extra_header` at `src/core/profiles.c:72-82`, which only appends. Lines 472 and 477 of `checkpoint.c` therefore leave two extra routing headers on the caller's context, and line 502 adds a third to the result. The identity digest then covers more headers than the saved one, so recovery for the grok proxy profile likely always fails. The existing test does not exercise this, judging by its non-proxy setup. One executed test would confirm it.

## Proposed migration for item 1

- **File split.** Keep the three encoders and the identity digest in C, since they own nothing. Move restore and recover into a new `src/core/checkpoint_restore.cpp` behind the unchanged C prototypes.
- **Context owner.** A unique pointer whose deleter calls `tny_ctx_free`, released only on full success. It replaces about 40 free-and-return sites.
- **Field table.** One table of key, member and secret flag drives all 27 strings. Every copy is checked and committed by swap.
- **Caller guard.** A scoped guard snapshots and restores both the model and the extra headers on the caller's context, on every exit.
- **Wiping owner.** Serialized JSON that contains credentials needs a wiping string owner, and none exists in `src/util/ownership.hpp` today. The helper at `checkpoint.c:101-106` currently releases settings bytes with plain `free`, while line 397 wipes the same data.
- **Allocator.** The Makefile strips the allocator override from C++ units. The new file must call `tny_alloc_strdup` and `tny_alloc_calloc` explicitly, or fault sweeps miss it. Both are free-compatible, so `tny_ctx_free` needs no change.

## Acceptance invariants

- **All or nothing.** Restore returns a complete context or NULL, and the encode, restore, encode round trip is byte-identical.
- **Arrays.** Counts equal committed entries at every failure point, and header arrays are always terminated. A non-string element fails the restore. An import order longer than four fails before any write into the array.
- **Borrowed JSON.** No pointer into the caller's document survives return. Test this by freeing the document first, under the sanitizer lane.
- **Caller context.** It is byte-identical after recover, whether recover succeeded or failed.
- **Recovery policy.** Unknown keys reject, the eleven private keys never override, and widened permissions reject. A changed identity rejects, a public null overrides, and absent keys fall back.
- **Cleanup.** Zero live allocations remain after every injected failure, and a clean retry then succeeds.

Tests:
- Add new `checkpoint-restore` and `checkpoint-recover` fault scopes that sweep every allocation index.
- Add a test that recovery succeeds for the grok proxy profile.
- Add five mutation targets in `tests/mutation/mutate.py`.

## ADR risks

- **Two ADR 0114 files bind.** One says credentials stay in wiped C owners and that checkpoint policy stays in C. A new ADR must carve out context reconstruction ownership only, with credentials held in C++ only by wiping transient owners.
- **Policy stays in C.** Keep the accept and reject predicate at `checkpoint.c:459-467` and the key allow-list visibly unchanged.
- **Behaviour changes.** Record the header fix and the eight checked copies as defects fixed, not refactoring.
- **MCP.** One of the two ADR 0114 texts keeps MCP in C, so item 3 needs its own carve-out.
- **Separate policy issue.** Recovery takes extensions, sandbox mode, extra directories, state directory, SSH host and max steps from the on-disk snapshot with no bound. A wrong-typed max steps falls to zero, which means unlimited. Pin current behaviour in tests and track this apart from the ownership PR.
