# ADR 0135: Clean disposable build variants

Date: 2026-09-18. Status: accepted.

## Context

Root-level `build-*` directories are ignored build and test outputs. Separate
provider, sanitizer, ownership, and SDK runs leave these behind because the
original `make clean` removed only `$(BUILD)` and `dist`. Twelve such directories
occupied about 504 MiB during inspection. Custom build lanes must still be
cleanable without removing other lanes.

## Decision

When `BUILD` is exactly `build` (its default), `make clean` also removes
root-level `build-*` directories. Skip matching regular files and symlinks;
do not recurse to discover other build roots. Quote each removal path and
propagate removal failures. An unmatched glob is harmless.

With any other `BUILD` value, retain scoped cleanup of that directory and
`dist`. Sibling apps keep their existing cleanup targets. Do not detect active
processes: callers must wait for builds/tests to finish, as with ordinary
`make clean`. Old test logs and reports are disposable, not archived.

## Verification

`tests/integration/test_make_contract.py` executes the real Makefile in
throwaway trees. It checks default cleanup, paths containing spaces, repeated
and empty cleanup, custom build isolation, and preservation of matching files,
symlinks, nested directories, and sibling-app output. The default cleanup test
fails against the old recipe because `build-acp` survives. The existing
integration runner discovers this test; no new runtime tool is required.
