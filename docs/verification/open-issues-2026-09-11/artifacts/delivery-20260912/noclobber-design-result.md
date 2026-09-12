## Approve conditionally — design only; primary writes remain blocked

The proposal is the smallest sound integration. It puts no-overwrite publication inside the existing image operation, so the guard, manifest, output and retained-error result refer to **one real destination**. It needs neither a new public ABI nor a new platform seam, subject to the conditions below.

**Do not implement or reuse it until the required A13 corrected-source review passes.** This review does not discharge that gate or A14’s separate corrected jobs review.

### Why the current wrapper must go

The jobs worker temporarily changes `r->output_file`, runs the service, then links and unconditionally unlinks that staging name. With manifests overlaid, this would:

- Record the staging destination rather than the final artifact.
- Put the shared service guard on the wrong name.
- Delete a committed paid artifact when service manifest finalization returns an error.
- Introduce another cancellation/publication boundary after the service has already committed.

Sources: jobs worktree `src/cli/cmd_image.c:32–68`; canonical `src/core/image_service.c:535–577, 626–679`.

### Minimal changes and approval conditions

| Change | Required condition |
|---|---|
| Private request `bool no_replace` | Default false. Set only from the existing private CLI prefix. Keep public grammar, tool schemas and SDK ABI unchanged. Jobs already emits the prefix when `overwrite` is false: jobs worktree `src/core/jobs.c:2804`. |
| Preflight under the canonical output guard | After guard acquisition, use `lstat` on the final destination. Proceed only on **ENOENT**; existing entries and other errors fail before HTTP. Do not use `access()` or treat all lookup failures as absence. Keep existing reserved-name/reference/alias checks. |
| Commit the existing validated temporary file | Use atomic `link(tmp, final)` for no-replace; retain `rename` for ordinary calls. `EEXIST` is a noncommitted failure, never permission to fall back to replacement. The guard alone does not exclude external writers. |
| Handle link cleanup correctly | Successful **link**, not subsequent temporary-name unlink, is the irreversible commit. Mark committed immediately. An unlink failure must not route through “nothing written” cleanup, delete the final output or skip truthful manifest finalization. Define honest cleanup reporting without misusing `IMAGE_MANIFEST_FINALIZE_FAILED`. |
| Preserve manifest and failure identity | Keep `r->output_file` unchanged throughout. Intent and terminal record name the canonical final output. On finalization failure, retain that output, operation identity and retained error. Preserve A13’s committed IO-before-late-cancel precedence; cancellation before commit publishes nothing. |
| Compute the digest before commit | Hash the exact validated buffer that is written, check hash failure before publication, and retain the digest in the **private result**, including `no_manifest`. Merely moving a local hash calculation is insufficient for ADR0097’s downstream identity check. |
| Keep platform handling in existing IO infrastructure | Any capability-specific handling belongs in `util/image_io`, not a new `#ifdef` in the service. If a filesystem cannot support atomic no-replace publication, reject that mode before HTTP. Never emulate it with check-then-rename. Ordinary wasm generation/manifests must remain usable; jobs remain explicitly unsupported. |

Current canonical code hashes only after commit and only with a manifest; it also has no result digest field. Sources: `src/core/image_service.c:380–400, 626–658`; `src/core/image_service.h:23–100`. Existing canonicalization and native/per-instance wasm guards are in `src/util/image_io.c:29–75, 176–241, 295–345`.

### Minimum integration checks to add or update

- **Existing destination:** private-prefix service invocation and real job both fail with zero provider requests; bytes remain unchanged. Include dangling symlink and normalized-path cases.
- **Destination appears during paid response:** deterministic provider barrier creates the destination; exactly one request, competing bytes unchanged, no committed-success artifact claim.
- **Success:** manifest filename, recorded artifact path, returned path, operation ID and independently calculated digest all identify the real final output. No `.job-*` publication or leftover stage.
- **Finalization fault:** real job and CLI retain final bytes and structured retained failure. Late cancellation does not replace committed IO detail; precommit cancellation publishes nothing.
- **No manifest:** both success and collision paths retain the same no-overwrite semantics; success supplies the exact private digest but writes no provenance record. Inject hash failure to prove it cannot commit.
- **Compatibility/platforms:** ordinary replacement and explicit job overwrite still work; wasm ordinary generation remains working, and unsupported private no-replace fails before spend.
- **Manifest-aware retry:** remove the jobs fixture’s manifest-null assumptions/skip; prove carried success validates the final manifest and output hash without regeneration.

Reuse `tests/integration/test_image_workflow.py:1005, 1466` and jobs worktree `tests/integration/test_jobs.py:1419–1447, 1503`; add focused commit/hash/cleanup faults in `tests/test_image_service.c`.

### Alternatives and remaining gates

- **Reject:** keep CLI staging and rewrite/move its manifest afterward. This adds another finalization window and still mishandles retained failures.
- **Reject:** rely only on job reservations or an absence check. Neither prevents an external destination appearing during HTTP.
- **Acceptable implementation choice:** a small internal IO publication helper can isolate link/rename and committed-versus-cleanup outcomes. Do not build a second transaction system. Parent-fd hardening can reuse the existing IO seam if required; this change must not claim protection against arbitrary parent-directory namespace replacement.

Contract basis: original issue bodies at `artifacts/original-issue-bodies.md:44–63, 113–133`; canonical contract A2 at `contract.md:300–308`, A11 at `461`, A13 at `502`, and A14 at `519`; producer identity requirement in `docs/adr/0097-explicit-generated-artifact-preview.md`.

**Verification status:** read-only source/design inspection. No edits, tests, children, goals or commits. A13 approval is not established here.
