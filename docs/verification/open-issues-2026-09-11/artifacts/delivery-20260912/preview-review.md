## Decision: Reject integration of the captured-queue prerequisite

The normal local path captures bytes and sends them through the existing attachment mechanism. However, terminal cleanup is incomplete. Some stopped previews remain in the transcript or queue and can reach a later turn. Payload bounds and control-field validation also need correction.

This is a **static, read-only review**, not a test result. No files were changed. No tests, agents, goals, or commits were created or run. I used the canonical #126 requirements/A15, original issue #126, project instructions/docs, and raw source/test files. No handoff, review reports, results, progress, or evidence files were used.

### Blocking findings

| Priority | Finding and actionable location | Required correction |
|---|---|---|
| **P1** | **Stopped previews are committed before the stop is checked.** `src/backends/openai/openai.c:1638–1652` flushes and saves the image message before checking extension stop, cancellation, denial, or step exhaustion at `1669–1695`. The later `preview_not_delivered()` only clears the pending queue. It does not remove the already committed image message. Thus a preview reported as not delivered can be sent as history on the next turn. | Decide terminal disposition before committing preview-bearing batches. For failures after message construction, retain enough ownership to remove an unsent preview message. Add a second-turn exact-byte assertion for cancellation, extension stop, denial, and exhausted budget. The existing early-stop test, `tests/test_openai.c:616–626`, ends without checking recovery. **A15 D2.** |
| **P1** | **Some terminal exits bypass preview disposition entirely.** A later tool’s persistence failure calls `emit_turn_end()` directly at `src/backends/openai/openai.c:1779–1790`. That function (`239–265`) neither reports non-delivery nor clears previews. `oa_send()` resets batch state but not the pending queue (`2064–2082`). Separately, a provider-request extension stop ends the turn and returns success at `1063–1074`, so `finish_tool_batch()` does not report the flushed preview as undelivered. Hard cancellation frees the engine at `src/core/runner.c:839–846`; destruction clears bytes but emits no preview disposition (`openai.c:2399–2405`). | Centralize terminal disposition for accepted previews, including pending bytes and constructed-but-unsent messages. Cover persistence failure after an earlier accepted preview, provider-request stop, and hard cancellation. Require explicit non-delivery before finalization and no stale attachment in a subsequent turn. **A15 D2.** |
| **P1** | **The 8 MiB bound is not enforced on the actual read.** `src/core/image.c:43–56` checks a pathname’s size, then calls `file_slurp()`. That reader reads until EOF without a byte limit (`src/util/util.c:292–310`). Concurrent file growth or replacement can therefore exceed the checked size. Admission retains the returned length without another bound check (`src/core/tools.c:822–851`); the loaded builder also accepts it (`src/core/image.c:160–181`). | Use a bounded reader on the opened file and reject oversized actual content before admission. Keep queue/transcript unchanged on refusal. Test controlled growth/replacement during loading, not only a file that is already oversized. The current size test at `tests/test_core.c:2682–2692` covers only the latter. **A15 payload bounds; R126.6.** |
| **P2** | **Control validation checks C-string prefixes, not complete JSON strings.** `src/core/runner.c:1012–1016` obtains id/path/hash with `jget_str()` and validates with `strlen()` and the NUL-terminated hash validator. `jget_str()` does not reject embedded NUL (`src/json/json.c:12–21`). Operation and role comparisons have the same problem (`runner.c:900–927`). Malformed values can therefore be interpreted as valid prefixes instead of being rejected. | Validate decoded JSON lengths and reject embedded NUL before op/role/id/path/hash interpretation. Require exactly 64 decoded hash characters. Apply length-aware correlation to replies at `src/cli/cmd_control.c:132–133`. Add malformed-field receiver tests. **A15 exact tool-role operation and id/path/hash validation.** |
| **P2** | **SSH remains a path-only exception to immutable capture.** `src/core/tools_ssh.c:633–642` discards the fetched bytes and queues only a staged pathname. `src/core/tools.c:724–744` rereads that file during flush. The exception is documented in `src/core/tools.h:80–86`, but canonical A15 requires accepted entries to own captured bytes and use those same bytes at flush. A unique filename is not immutable byte ownership. | Transfer the already fetched SSH bytes into the shared captured entry, with the same size/hash/cleanup rules, while preserving manual SSH behavior. Add a test that changes the staged source before flush. Alternatively, obtain an explicit canonical scope exception; the local documentation alone does not supply one. **A15 capture-on-admission requirement.** |

### What the inspected code does establish

| Area | Static assessment |
|---|---|
| Local captured bytes | One logical queue and count. Local manual and preview admission share `queue_capture()`. Hash comparison uses the captured bytes. Flush uses those bytes rather than reopening the source. |
| Admission | Engine checks active native ownership. Backend checks a live tool batch, cancellation/permission state, and remaining step budget. Unknown capability permits manual attachment but refuses preview. |
| Next provider request | Normal completion flushes after tool results and before `start_post()`. Tests contain independent base64/hash assertions for two versions of the same pathname, order, and mixed batches. |
| Incompatible mixed batch | Direct flush preserves entries and returns a distinct preview-fatal outcome. The normal fatal-flush owner path prevents the next POST and clears the batch. This does **not** cover the terminal paths above. |
| Control transport | Separate `image_preview` operation; no manual-attach fallback. Helper returns data without stdio, including wasm. Optional status/code fields preserve legacy reply fields. No acknowledgement retry. Client closes its fd and restores SIGINT handling on normal exchange exits. |
| Transport bounds | Client request/reply buffering is capped around 1 MiB; runner lines at 64 MiB. Connection establishment has a deadline. Reply waiting has no deadline and relies on SIGINT or socket closure. |
| Wasm | Shared manual capture tests are wired into the wasm CI command. The control helper returns no-socket/unsupported without printing. However, `ControlPreviewTests` explicitly skips wasm (`tests/integration/test_image_preview_queue.py:325–331`); these tests do not establish runtime coverage of the new wasm refusal helper. |
| Status and fallback | `queued` is documented as a time-local receipt, not perception. Capability refusal is separate from unavailable session and invalid image. The lifecycle findings prevent approval of end-to-end non-delivery semantics. |

### Prerequisite versus full #126

The absence of `generate/edit --preview`, derived preview sizing, full-resolution/preview result separation, and manifest lineage is **not a defect in this bounded first slice**. A15 explicitly defers those integrations.

Conversely, this slice cannot establish completion of #126’s CLI/tool generation parity, artifact-preserving fallback instructions, or separate generation/preview result statuses. Those remain later integration requirements.

**Recommendation:** correct the five prerequisite findings, then obtain a fresh review before reusing this lifecycle in generated-result preview integration.

### Inspected source hashes

SHA-256 of the working-tree files reviewed at the relevant implementation/test seams. Base HEAD: `b80c04b9df740c8388da03991cf4808c07e9cb50`. The review concerns dirty working-tree content, not HEAD alone.

```text
adee6eb8a1002f8da79fd78f365d7835b0bea9bf7816fb2d472a6e8e46bffbee  src/core/image.c
2d53c8675cf54eb868f221f866be27da8a038e8b85dca108967baf60f88fbcea  src/core/image.h
f40565d5dd4be2112c5df6fbc4cc5fab33b57ab46d1071cc737dd422fcb4c2ae  src/core/image_preview.c
8b124b6d1f45fa18b8c70e8b205e8ffff743e27573149302ff4ea3bd484b3203  src/core/image_preview.h
6ba575c46181942f927ae19a7580a471b32ee300b3699b5087991a56b0e05736  src/core/tools.c
2b658daad730ae9e25357e42c7411e736d847705a3c2f9c7c9226dc6a633fb9e  src/core/tools.h
380437004b927072e69be53576c01641c89f75aecfc80678b963792eee7424c7  src/core/tools_image.c
a64d8f06adc32284db9d2e65a61525185e732023a46f40ed403f4c1de4353198  src/core/tools_ssh.c
1dc39179efab4205ac56374f675d1f5af51e28f9e31e03603db9c57fc5c842de  src/core/tools_shell.c
5155dc383591654f4f4fac6350a5c2604546b36056cd20ce7effa0006a54fb60  src/core/runtime.c
bd410e2aa578b7f60f8893237da7d9aabf7acfa003f033a670a7a7fcdabd90b0  src/core/runner.c
5062e1bf629372a3bb48db645101679bde166e1278affdcad76bcc87f3a673eb  src/core/config.c
3706d7bb46e447e42ca1bd19368d7780f6558f03b7bbcb5dc8c9059e654a9ede  src/backends/openai/openai.c
c1add6b34826e2863f340d4b8806dc361066067c673cf9a468aeda42619746f6  src/cli/cmd_control.c
dd9cb06e3ec2905013be5c8eba935ccacbd2a448fd3fc1e8c969b892fa64affd  src/cli/cmd_control.h
e0073778180b50a7ad67f72e37d0043bbb9ef90a47a8c7ddf77d65236869de22  src/json/json.c
03f225d3362397f6e0d5b1b3e0e15e82e9a9605a89f46c26e48b3207a34f821b  src/util/util.c
ba278aac01463c0e29ac3bab5c53587ec3c8f7443d2831527e7d5652dc7b5542  src/util/image_io.c
59264481cc89c17b04399e97c7d8233b4fde9ceb09b2fac736e5747078f0d8f8  tests/test_core.c
0c601285e48e74049a519215b1e403c45ddc7aa701f2d051af4bcb8fd79bba4f  tests/test_openai.c
a0a5405989fe857ec9a5719925a93a9b33cbcfa8c3a7fef342f8d728a34ad34a  tests/test_runtime.c
9c48f458dd94505a613aa76a879b2cb602d869b14b5749840cc2b45ba52f5567  tests/test_runner.c
032f45440170b5142d6b1297fbace6b0c4e29db271974a2542381831a32da6f8  tests/integration/test_image_preview_queue.py
```

Canonical inputs, under `/Users/tomas/projects/tny-open-issues-2026-09-11/docs/verification/open-issues-2026-09-11/`:

```text
4d29e2d6fb187e48516d6b2bb707c16d3d7d3ad6d86850db93c28fa97e3664d3  contract.md
2d4a361207b7c35f769dc69315c545aa19614602b1ae511b7ce48a63258a528e  artifacts/issues.snapshot.json
```
