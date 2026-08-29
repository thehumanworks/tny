# 09 — Run `test_site_mobile.py` in a defined CI lane

Medium. From the repository-wide test audit. Independent of 08 except
that the wasm job is the natural home (it already installs Playwright).

`tests/integration/test_site_mobile.py` exits 0 when Playwright is
missing. Native `make test` and Nix both lack Playwright/Chromium, and
the wasm job installs Playwright but invokes only `test_site_wasm.py`.
The layout assertions at lines 68–116 never run.

## Work

- Invoke `python3 tests/integration/test_site_mobile.py` in the wasm CI
  job after Playwright/Chromium is installed
  (`.github/workflows/ci.yml` wasm “Browser smoke” step).
- Keep the skip-if-no-Playwright path for local/Nix fixture runs so
  `make test` stays runnable without browsers — but that skip must not
  be the CI path.
- If the mobile test needs the staged site (not just `site/`), share
  the docroot helper with 08/11 rather than copying `site/` a third way.

## Acceptance

- A forced failure in `test_site_mobile.py` (e.g. a missing
  `viewport-fit=cover` assertion) turns the wasm job red.
- Local `make test` without Playwright still exits 0 via the documented
  skip, not via a silent pass of empty assertions.
