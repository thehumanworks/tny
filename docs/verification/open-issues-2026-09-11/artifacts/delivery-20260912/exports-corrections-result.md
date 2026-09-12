I verified all four findings against A12 and the current code. I will keep the worker’s existing changes, bind approval to an owned export plan, and add regressions for each defect. I will stop before canonical integration.
The converter bounds and owned-plan fixes now build. The first regression run found only test assertion errors, which I am correcting. I also confirmed that export’s permission recheck rejects `ALLOW_ONCE`; I will bind that approval to the prepared export identity without creating a lasting grant.
Real ImageMagick 7 pixel checks now pass for exports and contact sheets. The timeout tests return within the 75-second limit for both continuous stdout and inherited writers; cancellation tests also pass. The first leak run failed because it inherited this agent’s tool-profile and nesting flags. The affected tests pass with those flags removed, and I will rerun the full leak gate that way.
The full export suite passed: 38 tests passed and the wasm-only case was skipped on this native build. Workflow checks passed 77/78 with one skip, and all 13 image-service checks passed. Compiled faults were detected for deadline, drain cancellation, source dimensions, and `ALLOW_ONCE`. A forced output-hash failure now preserves the old destination; the same fault in the original code did not.
Implemented the four confirmed A12 corrections. **Stopped for fresh review before canonical integration.** No commits, pushes, agent children, goals, or ADRs were created.

### Work completed

- Converter reads are nonblocking and bounded. Every drain iteration checks cancellation and the deadline, including after child exit. Version probing also supports cancellation.
- Execution uses an owned plan. Permission checks cover the same captured bytes and resolved lineage that the converter receives. `ALLOW_ONCE` remains scoped to one call, with typed/intercept parity.
- Derived manifests and reader roundtrips retain per-source dimensions. Returned metadata includes ordered source dimensions.
- Output hashing happens before irreversible commit. Remaining postcommit failures report retained artifacts truthfully.

All 14 changed files are worker-owned. Other repository files match their initial hashes.

### Checks and results

| Command / check | Result |
|---|---|
| `make -j8 debug release` | Passed. Stripped macOS binary: 969,744 bytes. |
| `env -u TNY_NESTED -u TNY_TOOLS make test-unit` | **494 passed**, 12,486 assertions. |
| `TNY_TEST_MAGICK=/opt/homebrew/bin/magick python3 tests/integration/test_image_exports.py "$PWD/build/tny" -v` | **38 passed**, one wasm-only skip. Includes real pixel/original-byte checks and both 75-second deadline cases. |
| `python3 tests/integration/test_image_workflow.py "$PWD/build/tny"`¹ | **77 passed**, one skip. |
| `python3 tests/integration/test_image_service.py "$PWD/build/tny"`¹ | **13 passed**. |
| Final approval and typed/intercept reruns | Passed. |
| `make quality` | Passed. GCC analyzer explicitly skipped on Darwin. |
| `env -u TNY_NESTED -u TNY_TOOLS make leaks` | Passed. Real export/contact-sheet leak checks also reported **zero leaks**. |
| Original / compiled-fault / restored checks | Original defects reproduced. Deadline, drain-cancellation, dimensions, and `ALLOW_ONCE` faults detected. Forced output-hash failure preserves the old destination in corrected code. |
| `git diff --check` | Passed. |

¹ These commands also removed `TNY_NESTED` and `TNY_TOOLS`.

The initial leak run failed on assertions affected by those inherited harness flags. The clean-environment rerun passed. The original continuous-SIGINT test could pass through an interrupted poll; the separate compiled drain-guard fault was detected.

### Exact changed files and SHA-256

```text
a53fd80f030ad63934983e3b7219f3ca811f3d09a35064eb6b9f7ff6ebe9b849  docs/images.md
8d6edf548d58181a7b572f5c1be699ffa8e9e911087a167cb9da431ad5423801  src/core/image_export.c
c708891022c80fd4bf31b69cd6ecbbb54d82f9017ed0c6b71dc40dd5e2f8a3e0  src/core/image_export.h
dc69240b6591662d7627c831cdd3363584c82d531d87fb27e6ef64ce089bca8d  src/core/image_manifest.c
643859b02d239729f806ae60bb4ea0911b13a1ae91da83b943352ba4250113a1  src/core/image_manifest.h
cc347bc31bd489bc0003d1e8df8050e206c99c5561785cdf1426e436584439ea  src/core/intercept.c
66b182749ae9d7cde451bf3d7ff7d6538e3063fd66680ec897ab050c64d324eb  src/core/tools.c
d83db40f4f82d3a75f8b5a1146383598d53c2ba903fee87933446f626b291126  src/core/tools_ext.c
5b2cbed746a249ba14374233e93c8bdb2587049c7eac27336a65f4908983521f  src/core/tools_image.c
099814631c8413b1d9823bb3d0737e7be2154bd8a8d6e5411a7bd87502111b29  src/core/tools_image.h
e217f52130cef204c6774ee757eeb8c0c1c9c5329c5009132c6c46ca1d244569  src/util/image_transform.c
99a979caae05fc59dfb6c087b5632d243d7117c114e193ab71c16c5edfc9989a  src/util/image_transform.h
5d3e66e6a96f135eaff96e2a820ae166ac04039eeb431de505f42ff09b25dc7a  tests/integration/test_image_exports.py
e419806d07b78156cf1d6a460fa43dee658aa6b44da975675484b379a6a0cbf0  tests/test_image_service.c
```

### Evidence and remaining gate

Evidence is in the canonical tree at:

`docs/verification/open-issues-2026-09-11/artifacts/delivery-20260912/export-corrections/`

It includes the report, exact commands/results, original snapshots, correction-only patch, fault logs, and source/evidence hash inventories.

**Remaining gate:** fresh independent actual-code review. Full monorepo `make test`, Linux, wasm, and Windows verification were not run in this bounded pass.
