# 08 — Gate Pages publish on CI-tested wasm

High. From the repository-wide test audit. Absorbs former tasks 09
(mobile smoke lane) and 11 (docs mirror manifest): all three edit the
same Pages/wasm CI path and are landed as one change.

`docs/ci.md` claims the landing terminal is the CI-tested artifact. The
Pages workflow contradicts that: it rebuilds wasm itself, commits to
`main` with `[skip ci]`, and never waits on the `ci` aggregate.

## Work

- Stop `.github/workflows/pages.yml` from publishing on an untested rebuild.
  Prefer consuming the exact `tny-wasm` artifact uploaded by
  `.github/workflows/ci.yml` (`build/wasm/tny-web.mjs` + `.wasm`) over a
  second `make wasm-web`.
- Publish only after the `ci` aggregate (`.github/workflows/ci.yml` job
  `ci`, needs quality/build/musl/windows/wasm/tsan/fuzz/tnytty) has
  succeeded for that commit. Typical shape: `workflow_run` on `ci`
  success for `main`, or a Pages job in the same workflow gated on `ci`.
- Browser-test the staged `docs/` docroot (not a temp copy of `site/`
  alone) before the push. Reuse or extend `tests/integration/test_site_wasm.py`
  so the published tree is the tested tree.
- Keep the `[skip ci]` bot commit from looping, but do not use it to skip
  the tests that already ran on the source commit.
- Update `docs/ci.md` so the “CI-tested artifact” sentence matches the
  new wiring.

### Mirror manifest (former 11)

- Record the generated site paths (HTML + `site/assets/**`, excluding
  engineering markdown that Pages must not overwrite) and assert the
  published `docs/` tree matches: every generated path exists, and no
  previously generated path survives after it is removed from `site/`.
- Change the Pages mirror from blind additive `cp -a site/. docs/` to a
  sync that deletes stale generated files. Never delete contract
  markdown under `docs/` (`docs/*.md`, ADRs).
- Point the browser smoke at the staged `docs/` docroot, not a temp copy
  of `site/`. One shared docroot helper; do not copy `site/` a third way.

### Mobile smoke lane (former 09)

- `tests/integration/test_site_mobile.py` exits 0 when Playwright is
  missing, and the wasm job never invokes it, so its layout assertions
  never run. Invoke it in the wasm job's “Browser smoke” step after
  Playwright/Chromium install, against the same staged docroot.
- Keep the skip-if-no-Playwright path for local/Nix runs so `make test`
  stays runnable without browsers — but that skip must not be the CI
  path.

## Acceptance

- A commit whose wasm/browser tests fail cannot publish Pages.
- The bytes under `docs/assets/wasm/tny-web.{mjs,wasm}` are the same
  bytes the wasm CI job uploaded, not a separately rebuilt pair.
- `docs/ci.md` no longer claims a CI-tested landing terminal while Pages
  rebuilds independently.
- Removing `site/docs/cli.html` (or an asset) from the source tree and
  running the publish/sync path leaves no `docs/docs/cli.html`;
  engineering markdown under `docs/` is untouched.
- A forced failure in `test_site_mobile.py` turns the wasm job red;
  local `make test` without Playwright still exits 0 via the documented
  skip.
