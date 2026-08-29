# 08 — Gate Pages publish on CI-tested wasm

High. From the repository-wide test audit. Independent of 09–14 except
that 11's `docs/` staging should ride the same publish path.

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

## Acceptance

- A commit whose wasm/browser tests fail cannot publish Pages.
- The bytes under `docs/assets/wasm/tny-web.{mjs,wasm}` are the same
  bytes the wasm CI job uploaded, not a separately rebuilt pair.
- `docs/ci.md` no longer claims a CI-tested landing terminal while Pages
  rebuilds independently.
