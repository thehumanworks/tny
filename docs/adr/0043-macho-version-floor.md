# 0043 — Mach-O current_version floors at 1.0.0 for pre-1.0 products

Date: 2026-08-28
Status: accepted

## Context

`libtny.1.dylib` links with `-compatibility_version 1.0.0` (ABI1). Mach-O
requires `current_version >= compatibility_version`, so
`scripts/check_abi_baseline.py --mach-version` rejected every product
version below 1.0.0. `--development-fallback` (always passed by the Makefile)
exempted only `git describe` development strings (`0.2.1-22-gHASH[-dirty]`),
so the first tagged release after the SDK landed (`v0.2.2`) could not build
`libtny` on macOS at all, and a dirty checkout at a tag could not either.

## Decision

With `--development-fallback`, any product SemVer below 1.0.0 — development
strings, `0.y.z` release tags, prereleases — maps to Mach-O `current_version`
`1.0.0`. Versions at or above 1.0.0 keep the monotonic
`monotonic-product-semver-numeric-triplet` rule from `abi/baseline-v1.json`.
Without the flag the checker still rejects pre-1.0 input, so the baseline
comparison remains strict.

## Consequences

- Every pre-1.0 macOS build of ABI1 carries `current version 1.0.0`;
  `tny_version()` / `runtime.libraryVersion` remain the real product version
  and the release job asserts on those, not on the Mach-O field.
- Once the product reaches 1.0.0 the Mach-O version starts tracking it
  monotonically with no further change.
