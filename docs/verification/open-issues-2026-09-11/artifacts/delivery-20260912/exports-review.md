The staging and destination commit paths use the main A12 safeguards. I found a timeout/cancellation defect and a source-dimension metadata gap. I am checking caller parity and test coverage before making the integration recommendation.## Decision: Reject for integration

The main A12 staging and destination safeguards are present. However, four source-level defects block approval: timeout/cancellation can stall, permission approval is not bound to the bytes actually converted, source dimensions are missing from metadata, and a post-commit hash failure misreports a retained artifact.

This was a read-only review against original issue #125, the prescriptive contract requirements/A12, relevant docs, and raw source/tests. No tests, builds, agent children, goals, edits, or commits were performed.

### Findings

#### 1. P1 — Converter output can bypass the deadline and cancellation checks

**Paths:** `src/util/image_transform.c:408–455`, also `:291–294`

When a pipe read returns bytes, line 419 immediately continues the loop. Continuous stdout therefore skips both cancellation and the wall-clock deadline.

After the direct child exits, lines 430–440 drain a **blocking** pipe without polling or checking the deadline. A descendant that still holds the write end can keep this read blocked. Direct-child exit does not guarantee pipe EOF.

The version probe also passes no cancellation callback, so an otherwise silent, stalled probe ignores cancellation until its deadline.

**Required change:** Use a nonblocking, bounded drain. Check cancellation and the deadline on every iteration, including after direct-child exit. Carry cancellation through executable probing.

**Test gap:** `tests/integration/test_image_exports.py:1053–1114` covers cancellation of a quiet sleeping converter. Add deterministic coverage for continuous stdout, an inherited pipe writer after direct-child exit, and cancellation during the version probe.

#### 2. P1 — Permission revalidation checks a discarded snapshot, not the converted inputs

**Paths:** `src/core/tools_image.c:297–313`; `src/core/image_export.c:386–430`, `:817–845`

The executor rebuilds the permission identity and checks the grant. But `tny_image_export_detail()` then frees the source buffers that supplied that identity.

The subsequent service call probes the executable, resolves the sources again, and reads fresh bytes. Nothing compares those fresh bytes and resolved records against the approved identity. A source or artifact record changed during this interval can therefore be converted under the earlier grant. The manifest-path permission check is similarly separate from the record resolution used by the service.

This does not meet A12’s exact grant revalidation and approved-byte staging requirements.

**Required change:** Build an owned execution plan containing resolved lineage, destination, and captured source bytes. Derive and check the permission identity from that plan, then stage those same buffers. Do not reopen records or sources after that check.

**Test gap:** `tests/test_image_service.c:789–797` checks a changed source during a new preparation call. It does not exercise a change between execution-time revalidation and the service’s later read.

#### 3. P2 — Export metadata drops every source’s dimensions

**Paths:** `src/core/image_export.c:336–344`, `:857–881`; `src/core/image_manifest.c:53–60`, `:84–87`

The source loader obtains width and height. The lineage construction copies only paths, hashes, and source manifest/operation IDs. Serialization consequently records the target canvas and sheet geometry, but no source dimensions.

For ordinary `--image` inputs, there is no source manifest from which those dimensions could later be recovered. This directly misses issue #125’s source-dimension requirement and **R125.3/I125.3**.

**Required change:** Preserve per-source dimensions in the derived transform schema and reader. Keep them distinct from the derived canvas and any native-generation dimensions.

**Test gap:** `tests/integration/test_image_exports.py:748–830` checks hashes, IDs, policy, and target dimensions, but never asserts source dimensions. Add round-trip assertions for both direct-file exports and mixed-source sheets.

#### 4. P2 — Hash allocation failure after commit falsely reports that nothing was written

**Paths:** `src/core/image_export.c:945–957`, `:997–999`, `:1067–1077`; `src/util/util.c:435–437`

The destination is committed before its SHA-256 is computed. The hash implementation allocates a padded copy of the output and can fail.

On that failure, `result->committed` remains true, but the retained-result predicate accepts only manifest-finalization failures. CLI error serialization then emits:

```json
{"path":null,"sha256":null,"committed":false}
```

Typed tools likewise miss the retained-artifact response. This can happen with `--no-manifest` too. The output exists—and an explicit overwrite may already have replaced the old destination—despite the response saying otherwise.

**Required change:** Hash the validated output before the irreversible commit. Also make retained-result handling cover any failure that can still occur after commit.

**Test gap:** Add a targeted allocation-failure check at output hashing. It should either fail before commit and preserve the old destination, or report the retained artifact truthfully.

### Assessment of the remaining design

| Area | Static assessment |
|---|---|
| Converter staging and coders | Private `/tmp` staging, generated ASCII filenames, forced input/output coders, finite options, and fixed bitmap labels are present. User filenames do not enter converter expression syntax. |
| Decoder validation | The producing invocation is followed by a second forced-coder decode with `-regard-warnings`, then MIME and exact-canvas checks before destination staging. |
| Exclusive destination and originals | Retained-parent-fd `openat` staging, exclusive/no-follow creation, `linkat` no-overwrite installation, `renameat` explicit replacement, alias rejection, and target identity rechecks implement the principal A12 mechanisms. |
| Lineage | Derived artifacts carry `native:false`, ordered source hashes, and artifact-record IDs. Existing artifact-reference handling preserves the derived record link. Source dimensions remain missing. |
| Platform seams | Process execution uses the host process/poll seams. Wasm service resolution refuses transforms before input/output work; local tools are gated for wasm, SSH, and library runtimes. No platform build was verified. |
| Tests and simplicity | Real-converter pixel checks cover fit/crop/pad, order, labels, preservation, formats, and failures. Shared service/IO boundaries avoid duplicating the provider loop. Repeated source resolution and hashing add complexity without securing the approval boundary; an owned plan would simplify this. |

**Recommendation:** Correct the four findings and add their focused regression checks before another independent integration review. Existing test source is useful coverage, but it is not execution evidence.

### Inspected source hashes

SHA-256 values captured at the end of inspection. These identify working-tree files, not merely HEAD.

```text
476ef67cab9187e7d6340cdac98b0a73b0033e4dac86b2db03214d9ad111fb04  src/core/image_export.c
8deac27e66a22227fb628b2ead724b94f42b3da9ea637890736d32e5dcc19487  src/core/image_export.h
dc802c5b05f9598fdba8182d660beae4287d0ec2fb44d1b146824a19e83ca986  src/util/image_transform.c
017266a0e64ff6ac452a2dc0d592d137cc0f4a955e741f4fbebd04b6a8cc5b8b  src/util/image_transform.h
a5afd6622793b1d763ffe775e83acabfff191ad0e18c8cba374c51063461fbf2  src/util/image_io.c
5ccaee3e8aaa191bb1ebddfe9755fd3733fd045c9fcd8ab1ddc4feeea65f1619  src/core/image_manifest.c
373ffe79b0085df1967d6046b259288789fd87f5ebce81bc3b5f0843d381a2f3  src/core/image_manifest.h
d425650ac32c66b5d60deaa6bf3afba716c26acab78eebb4f556b52944d96204  src/core/tools_image.c
b5b8d3146c22c215c5299c2327508ea62fc31dcc4b99bd79e46465ca8009bf03  src/core/intercept.c
6ae25fb819059b3a15c1742452175bdcfc489004b98d3abcbfc828f0ce142bbe  src/core/tools.c
05d50705b74f017c4d074c9afa98f9662bc6f89ad89147dffb50e3ebecda4440  src/cli/cmd_image.c
d878cbb29621d733c64593fc188f07f3045c335ec614e90913ce8011e5ebb794  src/util/process.c
03f225d3362397f6e0d5b1b3e0e15e82e9a9605a89f46c26e48b3207a34f821b  src/util/util.c
c55df484f5ae4d1e703692360422a8f98ef52c801391df70cb57df03a584e7bb  src/core/image.c
ddbdc21d5f68a61dce9fa463c2242fe9939c24d05cd0c77b90862c8c53fd1dd3  src/core/image_dimensions.c
b67b2df5afec1e058a47620cdd990aeff1f56596301ca85f79081a4a941e9352  src/core/image_service.c
9c5641d3ecab9e740bb418c6e6d17458e71aa6ea04304ec20d3f529099ec347a  src/lib/toolkit.c
55d2e7bd7da66a360e944adcfe04ee14b085d845a0ca7b16be58710dc33b4a9f  tests/test_image_service.c
777c4381a38cde07025b8fbfdf4376685b13013957e468c4a1aea1b6173f1370  tests/test_core.c
61d75d73b206b5f3e8e3a0c71a471f4edabd6b2a06c8dea833204628227b9623  tests/test_intercept.c
ab1c85c1ea78c2cf11739f240ad988f070562f3c43e647769aef7e513ab43f89  tests/integration/test_image_exports.py
```
