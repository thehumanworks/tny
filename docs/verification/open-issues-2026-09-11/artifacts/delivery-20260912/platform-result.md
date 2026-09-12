I found the platform context under the verification records. I will freeze the current canonical source, keep all runs isolated, and label the results as preintegration. I will preserve the original dictation failure and use the real fixture to investigate it.

The real Chromium backpressure test passed on the frozen preintegration source. The historical “dictation timeout” attribution needs correction: the cited P03 log shows all 36 dictation cases passed. That full run hit its outer 300-second deadline later. I am checking source and log hashes before recording that conclusion.

All 36 current dictation cases passed with the real fixture in 26.9 seconds. The source was unchanged. A separate browser probe also confirmed the exact native-only subagent refusal, with no child request. The older test labeled “unsupported subagent” only checked doctor capability reporting, so I will record that coverage gap.

The frozen preintegration Linux full suite passed in 524.3 seconds, including all 36 dictation cases again. I saved the source manifests, raw results, exact tool versions, failed preparation attempts, and reusable commands under `platform-preparation/`. Final integrated gates remain unrun; the results do not cover worker changes that are still separate.

Prepared **source-bound preintegration evidence and reusable run commands**. No canonical or worker source was changed. **No final gate or platform completion is claimed.**

| Work completed | Checks and results | Blockers / limits |
|---|---|---|
| Investigated historical dictation timeout | Original P03 log shows **36 dictation tests passed**. The full suite hit its outer 300-second deadline later. | No dictation code correction is justified. Keep every fixture and use the recorded in-container full-suite deadline. |
| Tested frozen canonical Linux source | Dictation **36/36 passed**. Full `make -j4 test` passed in **524.3 s**, including **491 unit tests**. Source hashes remained unchanged. | Ordinary skips remain explicit in the evidence. Preintegration only. |
| Executed actual Chromium WASM tests | Backpressure passed. Actual subagent call returned the exact native-only refusal, with no child request. Unknown image-provider refusal also passed without an image request. | Integrated export/jobs/preview cases are not present in this frozen tree. |
| Executed WASM image suites under Node | Image service, dimensions/manifests, and image-input suites passed their applicable cases. Native-only skips are recorded. | Node results are separate from browser evidence. |
| Prepared platform tooling and copy recipes | Exact wasm/browser/Nix/ImageMagick versions recorded. Real tracked/untracked Git inventory copy verified in both containers. | Nix reports `sandbox=false`; no Nix build/check was run. |

**Evidence and reusable commands:**
- [Preparation report and final-run procedure](docs/verification/open-issues-2026-09-11/artifacts/delivery-20260912/platform-preparation/README.md)
- [Machine-readable result](docs/verification/open-issues-2026-09-11/artifacts/delivery-20260912/platform-preparation/result.json)

Failed preparation attempts remain preserved with their corrections. The next run requires primary’s final frozen integrated tree.
