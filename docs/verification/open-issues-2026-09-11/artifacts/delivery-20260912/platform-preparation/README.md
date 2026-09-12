# Platform preparation — preintegration only

Prepared against HANDOFF and C-G5 on 2026-09-12. **No final gate or platform completion is claimed.** Primary has not supplied the final combined tree. No canonical or worker source was changed. No goals, delegated agents, commits, pushes, live credentials, or preexisting process termination were used. Test-created subprocesses use fixture-only credentials. Only this evidence directory and new `/work/platform-preparation-*` scratch in the two authorized containers were written.

## Source identity

- Baseline HEAD: `b80c04b9df740c8388da03991cf4808c07e9cb50`.
- Executed source: `frozen-preintegration/source.tar`, 1,102 files/links: 1,068 real tracked paths and 34 real untracked product paths.
- Inventory SHA-256: `8b4b365d964018d9b9136ac78ed86ded40bfbe23cbab428efe74f307a3af7677`.
- Manifest SHA-256: `68cf3c1755c9be883c3ee7aa16390d3bbce8c31b872acff64590ba0bb1c0ce56`.
- `manifest.json` records each path, bytes, mode, symlink kind, tracked status, exclusions, Git status, version and archive hash. `docs/verification/` is excluded as evidence/scratch, not product source. No ignored host build, login/config directory or worker tree is copied.
- Native and wasm builds use separate source copies in `/work/platform-preparation-20260912-1318`. SHA checks run after successful tests. `qa-archive.json` independently verifies every archive member, including symlink targets and modes.
- `prepare.py` reconstructs the **actual tracked inventory**, not `git add .`, in scratch-only indexes. The 34 untracked inputs remain untracked and are visible to Makefile discovery. There is no fabricated commit or remote. Version is explicit; the recorded baseline and manifest, not scratch HEAD, identify the build.
- A later freeze-recipe QA observed one new canonical file, `docs/adr/0097-explicit-generated-artifact-preview.md`. See `canonical-drift.json`. That later tree was **not** used for the reported builds/tests.

## Current executed checks

| Check | Result and limits | Evidence |
| --- | --- | --- |
| Native GCC release and real dictation fixture | Built; stripped native binary 986,032 bytes. Size observation only, not delivered-target acceptance. | `native-dictation.log` (build portion), binary hashes in corrected run |
| Exact canonical dictation suite | **36/36 passed**, 26.949 s; no fixture exclusion or source correction. | `native-dictation-correct-invocation.{json,log}` |
| Full Linux `make -j4 test` | Exit 0, **524.276 s**; 491 unit tests/12,267 assertions; dictation 36/36 again, 26.973 s. Source hashes unchanged. | `native-full-test.{json,log}` |
| `make wasm wasm-web` | Both actual artifacts built with Emscripten 6.0.8; hashes retained. | `wasm-build-browser.{json,log}` |
| Actual Chromium output backpressure | Passed: 23 false polls, first write 1,286 ms, three ordered events, one provider request, one terminal, exit 0, no diagnostic noise. Final helper reruns also pass. | `wasm-build-browser.log`, `browser-reusable-recipe-qa.{json,log}` |
| WASM image service, Node runtime | 13 run, 11 passed, 2 explicit native-only skips. | `wasm-image-service.{json,log}` |
| WASM image dimensions + manifests, Node runtime | 44 run, 35 passed, 9 explicit native-only skips. | `wasm-image-workflow.{json,log}` |
| WASM image-input policy, Node runtime | 8 run, 7 passed, 1 native ACP-process skip. | `wasm-image-input.{json,log}` |
| Actual browser subagent refusal | Real fixture requests `subagent.create`. Exact `SUBAGENT_UNSUPPORTED_CONTEXT` result, `tool_ok:false`, no child request, two parent requests, normal parent completion. | `browser-unsupported-exact-contract.log`, final QA recipe log |
| Actual browser unknown image adapter refusal | Exit 1; `available:false`, exact `unknown image provider` diagnostic, no image network request. This is not native export/jobs coverage. | Same browser probe logs |
| Doctor capabilities in WASM | Existing `test_extension_capabilities.py` passes. **It does not invoke subagent.** Earlier P12's “unsupported-subagent” name overstates this coverage. The current similarly named `wasm-unsupported-subagent` record also only runs that doctor test; use the actual browser probe for subagent proof. | `wasm-unsupported-subagent.{json,log}` and source inspection |
| Copy recipe | Corrected recipe executed in both containers: real tracked/untracked inventories and all file hashes matched. | `prepare-recipe-corrected.log` |
| Evidence-helper QA | Ruff check/format, Python syntax/archive verification, Bash syntax and ShellCheck passed. SC1091 excluded only for the installed external emsdk script. | `qa-*`, `recipe-shellcheck.*` |

The full native run retains its ordinary skips: Darwin-only slow-lock injection, unavailable OS sandbox wrapper, optional jsonschema, optional site mobile/browser tests (Playwright is in a separate venv), and the explicit browser-backpressure NOT RUN marker. A standalone fault-script invocation also prints its Make-target requirement. These messages are not counted as coverage. The separately required browser backpressure and browser refusal probes really ran, but they do not substitute for the site-wide browser suite.

## Historical timeout resolution

`historical-linux.json` stores the original metadata, log hashes and dictation excerpts. All three historical log hashes still match their recorded hashes.

1. `resume-20260911/integrated-platform-a6a4/P03-linux-full-test` hit its **outer 300.008-second full-suite deadline**. Its original log explicitly shows **36 dictation tests passed in 27.067 seconds**, then later tests/builds. The log ends at `test_net_host_safety.py`, not dictation. The record's “timed out in a dictation fixture” summary is not supported by its cited raw evidence.
2. The original longer-deadline retry completed the full suite in **489.306 seconds** with a 900-second in-container deadline. Dictation passed in 26.805 seconds.
3. The historical LTO retry completed the full suite in **533.850 seconds**. Dictation passed in 27.017 seconds.
4. `historical-dictation-source-comparison.json` proves that both historical manifests match today's frozen bytes for `test_dictation.py`, its `test_tui.py` helper, `dictation.c`, `dictation_xai.c` and `audio_capture.c`.
5. Current exact frozen dictation and full Linux runs pass, as above. No attributable dictation regression was reproduced. A pristine rebuild or product patch is therefore not justified by this evidence.

**Minimal proven correction:** correct the summary's attribution and apply a full-suite-appropriate **in-container** deadline. Keep the real `dictation-fixture` prerequisite and every test. The final recipe uses `tini -s -- timeout --kill-after=5s 1200 make -j4 test`; the historical 900-second retry and current 524-second run prove that a 300-second whole-suite cap is inadequate here. This is test orchestration, not a relaxed fixture assertion. Keep the original timeout as a failure; do not relabel it passed.

## Explicit failed preparation attempts

All raw failures remain; none are platform regressions or passing gates:

- `native-dictation`: five errors because this agent appended `-v`. `test_dictation.py` ignores CLI flags but imports `test_tui.py`, which treats `argv[1]` as the executable. Removing the unsupported extra argument, **without changing test bytes**, passed all 36 cases.
- `tools-nix`: `/bin/bash` does not exist in the Nix container. Corrected to `/root/.nix-profile/bin/bash`.
- `browser-unsupported-actual`: Docker preserved a 0600 host script owned by UID 501; container UID 1001 could not read it. Corrected only the task helper's transfer mode.
- `browser-unsupported-readable`: subagent refusal passed, but the new evidence probe guessed image diagnostic wording/channel. Actual clean failure was `unknown image provider`, delivered through the web module's output callback. The probe now asserts that exact result; source was not changed.
- `prepare-recipe-check`: Nix's minimal profile lacks `cmp`. The recipe now compares SHA-256 of sorted NUL-delimited inventories, using installed coreutils. It was rerun successfully in fresh scratch in both containers.
- Initial helper Ruff check found import order and a loop-lambda binding warning. Corrected, formatted and reran QA and the actual browser recipe. `scripts-before-qa.tar` preserves the earlier helper versions.

## Installed tools

Exact invocations, resolved paths and versions are in `tools-linux*`, `tools-nix-correct-shell.*`, and `tools-macos-magick.json`.

| Tool | Installed location/version |
| --- | --- |
| Linux | aarch64, kernel 6.8.0-117-generic; GCC Ubuntu 13.3.0-6ubuntu2~24.04.1; Clang 18.1.3; Make 4.3; Git 2.43.0; Valgrind 3.22.0; tini 0.19.0 |
| Emscripten | `/work/emsdk-6.0.8/upstream/emscripten/emcc`, 6.0.8, commit `aeb67926e7de656da38bc807d83050af93578758` |
| Node | Actual test launcher `/usr/local/bin/node`, 26.8.2; emsdk compiler's bundled Node `/work/emsdk-6.0.8/node/24.19.0_64bit/bin/node`, 24.19.0 |
| Python/browser | `/opt/tny-tools/bin/python3`, 3.14.7; `/work/tny-browser-venv-20260912/bin/python`, 3.14.7, Playwright 1.62.0; Chromium `/home/tny/.cache/ms-playwright/chromium-1234/chrome-linux/chrome`, 151.0.7922.34 |
| Quality | clang-format 23.1.0; clang-tidy 22.1.8; Ruff 0.16.6; ShellCheck 0.11.0; shfmt 3.14.0; actionlint 1.7.12 |
| Linux ImageMagick | `/work/imagemagick-7.1.2-31/bin/magick`, 7.1.2-31 Q16 aarch64, GCC 13.3; jpeg/png/webp delegates present |
| Mac ImageMagick | `/Users/tomas/.cache/tny-verification/open-issues-20260911/imagemagick-macos/prefix/bin/magick`, 7.1.2-31 Q16 aarch64, Clang 21.0.0; jpeg/png/webp delegates present. Read-only version discovery, not Mac runtime acceptance. |
| Nix | `/nix/var/nix/profiles/default/bin/nix`, Nix 2.35.2; aarch64-linux local store reachable. Current container configuration is **sandbox=false**. No Nix build/check was run here; do not call this hermetic verification. |

## Reusable final-run procedure

`freeze.py`, `prepare.py` and `final_lane.sh` are reusable command helpers, not final-gate verdicts. **Do not reuse `run.py` for final evidence:** it deliberately binds to this preintegration manifest and version.

On the Mac, after primary supplies and stops changes to the final tree:

```bash
P="$PWD/docs/verification/open-issues-2026-09-11/artifacts/delivery-20260912/platform-preparation"
FINAL_TREE=/absolute/path/supplied/by/primary
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
FROZEN="$P/final-candidate-$STAMP"
SCRATCH="/work/platform-preparation-final-$STAMP"
python3 "$P/freeze.py" "$FINAL_TREE" "$FROZEN" final-candidate
python3 "$P/prepare.py" "$FROZEN" "$SCRATCH" > "$FROZEN/copy.log" 2>&1
# Use the contract-pinned ABI archive, not an arbitrary checkout fallback.
git -C "$FINAL_TREE" archive 510a95c2ef89aa9ec02a66d8b0a5cadd953025a8 > "$FROZEN/abi0.tar"
test "$(shasum -a 256 "$FROZEN/abi0.tar" | cut -d ' ' -f1)" = 8718336dbde47f3f8427bf6b3a724127e3ed24b61eaedb6f315523ec2a00c2f6
VERSION=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$FROZEN/manifest.json")
D=(docker --context colima-tny-open-issues-20260911)
# Transfer helpers as readable files without changing their source content.
chmod 644 "$P/final_lane.sh" "$P/browser_unsupported.py"
for C in tny-verification-20260911 tny-nix-verification-20260911; do
  "${D[@]}" cp "$P/final_lane.sh" "$C:$SCRATCH/final_lane.sh"
  "${D[@]}" cp "$P/browser_unsupported.py" "$C:$SCRATCH/browser_unsupported.py"
  "${D[@]}" cp "$FROZEN/abi0.tar" "$C:$SCRATCH/abi0.tar"
done
```

Run each lane separately and retain its exit status even when another lane fails. Example (Bash on the Mac):

```bash
run_lane() {
  lane=$1
  C=tny-verification-20260911
  SHELL_PATH=/bin/bash
  if [ "$lane" = nix ]; then
    C=tny-nix-verification-20260911
    SHELL_PATH=/root/.nix-profile/bin/bash
  fi
  rc=0
  "${D[@]}" exec "$C" /usr/bin/env -i \
    PATH=/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 \
    "SCRATCH=$SCRATCH" "TNY_VERSION=$VERSION" \
    "$SHELL_PATH" "$SCRATCH/final_lane.sh" "$lane" \
    > "$FROZEN/$lane.log" 2>&1 || rc=$?
  printf '%s\n' "$rc" > "$FROZEN/$lane.exit"
  shasum -a 256 "$FROZEN/manifest.json" "$FROZEN/$lane.log" > "$FROZEN/$lane.sha256"
  return "$rc"
}
run_lane native-test
run_lane native-quality
run_lane native-abi
run_lane native-sdks
run_lane native-leaks
run_lane wasm-build
run_lane wasm-tests
run_lane browser
# Nix builds write the Nix store. Run only in primary's final-run scope.
# The helper requests sandbox=true; a sandbox capability failure is a blocker.
run_lane nix
```

The native recipe keeps real Git inventory and ABI inputs, uses no credentials, retains all dictation cases, and writes Emscripten cache only inside task scratch. Native-test and wasm-build should precede their dependent tests. Run mutating build lanes sequentially per source copy. Do not treat a skip as an acceptance pass.

**Still required from the final tree:** integrated exports/sheets/jobs/preview fixtures and their explicit WASM rejections, cross-feature/manifest-permission tests, ABI/SDK/quality/leaks/Valgrind and Nix results, full site browser tests, and other platform/live gates owned by primary. Those new integrated fixture names must be taken from that tree's CI and contract, not guessed here. Native Linux aarch64 and Chromium are not Windows/MSYS2, macOS, native x86_64, or live-provider proof. The final frozen tree is the only outstanding input needed for another platform run; this preparation is not platform completion.
