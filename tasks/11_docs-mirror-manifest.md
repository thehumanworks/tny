# 11 — Test the published `docs/` tree, fail on stale mirrors

Medium. From the repository-wide test audit. Composes with 08 (Pages
should publish the tested docroot).

Browser smoke copies and serves `site/`
(`tests/integration/test_site_wasm.py`). Production does additive
`cp -a site/. docs/` (`.github/workflows/pages.yml`). Nothing checks a
manifest of generated `docs/` paths; `test_site.py` only looks at
`docs/sitemap.xml`. Deleting a page or asset from `site/` leaves the old
copy publicly served.

## Work

- Record the generated site paths (HTML + `site/assets/**`, excluding
  engineering markdown that Pages must not overwrite) and assert the
  published `docs/` tree matches: every generated path exists, and no
  previously generated path survives after it is removed from `site/`.
- Change the Pages mirror from blind additive copy to a sync that
  deletes stale generated files. Do not delete contract markdown under
  `docs/` (`docs/*.md`, ADRs).
- Point the browser smoke at a staged `docs/` docroot (or the same
  tree Pages would push), not a temp copy of `site/` alone.
- Keep `test_site.py`'s sitemap check, but do not treat sitemap presence
  as proof the rest of `docs/` is current.

## Acceptance

- Removing `site/docs/cli.html` (or an asset) from the source tree and
  running the publish/sync path leaves no `docs/docs/cli.html` (or
  matching asset).
- `test_site_wasm.py` would catch a missing `docs/assets/wasm/` the same
  way it catches a missing staged wasm file today.
- Engineering markdown under `docs/` is still not overwritten.
