# 10 — Derive site version/size from the real binary

Medium. From the repository-wide test audit. Independent.

`scripts/site_build.py` hardcodes `VERSION = "0.1.0"` and `SIZE = "0.41mib"`
and a second “0.41mb” literal in the landing features list. The checkout
is past `v0.2.2` and the matching local binary is ~0.585 MiB. Generated
`site/index.html` publishes the stale values. `tests/integration/test_site.py`
checks structural literals only; the wasm browser smoke only asserts the
banner contains “tny”.

## Work

- Stop hardcoding version and size in `scripts/site_build.py`. Derive
  version from the same `git describe` / `TNY_VERSION` path the binary
  uses (ADR 0014) and size from a stripped Release `wc -c` (MiB, one
  decimal, consistent units — do not mix `mib` and `mb`).
- Feed that derived size into both the metadata and the “Tiny … binary”
  landing copy so the two cannot drift.
- Extend `tests/integration/test_site.py` to fail if the published
  version/size disagree with `tny --version` and the stripped binary
  byte count (tolerance only for rounding to the displayed unit).
- Optionally assert the wasm banner version matches the same derived
  string in `tests/integration/test_site_wasm.py`.

## Acceptance

- Regenerating the site on this checkout does not print `0.1.0` or
  `0.41mib` / `0.41mb`.
- Changing the binary size or git describe without updating the site
  generator makes `test_site.py` fail.
- Units on the landing page match the generator (one spelling, one
  magnitude).
