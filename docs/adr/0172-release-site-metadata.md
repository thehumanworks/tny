# 0172 — Site metadata follows the release tag and measured binary

Date: 2026-09-25
Status: accepted; extends ADR 0014 and ADR 0085

## Context

`site_build.py` published `v0.3.0` and a `0.71mib` historical size long
past that release. Other parts of the site rightly retain dated v0.3.0
benchmark numbers; labeling one of those as the *current* binary was wrong.
Pages commits generated content from `main:/docs` before the release tag is
necessarily cut. Publishing the development `git describe` suffix would
leave Pages behind the tagged release.

## Decision

Site generation reads `TNY_VERSION` when set, otherwise `git describe
--tags --always --dirty`, just like the Makefile, and measures bytes from the
stripped native `build/tny` (one-decimal MiB = 1,048,576 bytes). `make site`
first builds the release binary. Missing binaries fail instead of showing
stale metadata. The dated v0.3.0 comparison stays explicitly historical.
`test_site.py` checks the generated landing page and size page against the
binary version and its exact byte count, not a hardcoded release number.

Pages selects the most recent reachable stable release tag for `TNY_VERSION`
and builds a matching native release before rebuilding/mirroring the site.
The first Pages run on a new commit may still show the *previous* published
release; after the new tag is published, dispatch `pages.yml` on `main` to
republish the new version. Neither a Pages mirror commit nor an untagged
commit is falsely called a release. The wasm terminal continues to be built
from the same source on the Pages run.

## Verification

`make site`, `python3 tests/integration/test_site.py`, `make quality` and
`make test` are the local gates. The CI full-suite lane must install Z3 for
`make verify-formal`; the Linux analyzer must also accept the typed JSON key
check. Verify the release workflow's version job and GitHub Release after
tagging; dispatch Pages and inspect the published `docs/index.html` mirror.
