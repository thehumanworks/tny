# 0014 — Version is derived from git at build time, not hardcoded

Date: 2026-08-22
Status: accepted

## Context

`TNY_VERSION` was a hardcoded `#define` in `src/core/config.h` ("0.1.0").
It was not bumped when `v0.1.1` was tagged and released, so shipped
binaries, `tny --help`, `tny --version`, the TUI banner, the doctor/status
JSON, host handshakes (codex, ACP, MCP `clientInfo`), and the HTTP
`User-Agent` all reported the wrong version. The release workflow's
tag-vs-`config.h` string comparison only protects releases where someone
remembered the bump commit; it cannot fix dev builds, and it makes every
release a two-step ritual (bump + tag).

## Decision

The git tag is the single source of truth; no version string is committed.

- `make` runs `git describe --tags --always --dirty`, strips the `v`
  prefix, and writes `#define TNY_VERSION "…"` to
  `build/generated/tny_version.h`. A build at a tag reports exactly
  `0.1.1`; between tags it reports `0.1.1-5-g1a2b3c4[-dirty]`; a repo with
  no reachable tag reports the short commit hash.
- The header is regenerated on every make run but rewritten **only when
  the content changes** (write-to-temp + `cmp`), so cached objects
  survive; `-MMD` dependency files rebuild the version's users when it
  does change. Object pattern rules take the header as an order-only
  prerequisite so first builds sequence correctly.
- `src/core/config.h` includes the generated header via
  `__has_include`, with a `0.0.0-dev` fallback so editors and static
  analysis work without a build. `src/net/http1.c` includes it the same
  way directly, because net code must not include `core/` headers.
- `TNY_VERSION` is overridable (`make TNY_VERSION=x.y.z` or environment).
  Release CI passes `TNY_VERSION=${GITHUB_REF_NAME#v}` explicitly: shallow
  checkouts have no tags for `git describe`, and the Alpine musl container
  has no git at all. Builds from a git-less source tarball fall back to
  `0.0.0-unknown`.
- The release workflow's `version` job now builds the binary and fails
  unless `tny --version` prints the pushed tag, replacing the
  tag-vs-source string comparison.

## Consequences

- Releasing is one step: `git tag v<version> && git push origin
  v<version>`. Nothing to bump, nothing to forget.
- Every dev binary is traceable to a commit, and `-dirty` marks uncommitted
  state — more useful bug reports than a stale constant.
- The first make run after a commit (hash change) or a clean/dirty
  transition rebuilds everything that includes `config.h`. The tree builds
  in seconds, so this is accepted rather than narrowing the include graph.
- Tests assert shape, not a constant: the unit suite checks the compiled
  string is non-empty, un-prefixed, and printable
  (`version_string_is_sane` in `tests/test_core.c`); integration tests
  read the version from `tny --version` and compare it to `git describe`
  output rather than a literal.
- The static site (`scripts/site_build.py`) still carries its own version
  string; it is regenerated content, out of scope here.
