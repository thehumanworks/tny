# 0045 — The repo becomes a monorepo; tnytty is the first sibling app

Date: 2026-08-29
Status: accepted

## Context

tny needs a terminal it can call its own: **tnytty**, a tiny terminal
emulator built on the same principles as the harness — C11, smallest
binary, microsecond startup, cross-platform, scriptable. tnytty shares
tny's vendored libraries (yyjson, picohttpparser, greatest), its quality
gates (docs/adr/0039), and its docs-first contract style, so it belongs
in the same repository.

The obvious shape — move the harness into `tny/` and the terminal into
`tnytty/` — breaks hard external constraints:

- GitHub Pages deploys from `main:/docs`. `docs/` is both the
  implementation contract and the published site mirror
  (`.github/workflows/pages.yml`); it cannot move without repo-settings
  changes and a window where the site 404s.
- `release.yml`, `sdk.yml`, and `source-snapshot.yml` pin root paths
  (`Makefile`, `sdk/`, `abi/`, `scripts/`), as do the pip/npm packaging
  manifests and the ABI-baseline checker.
- The nix flake filesets (`nix/source.nix`) and every consumer of
  `nix run github:thehumanworks/tny` assume the flake at the root.

## Decision

The repository is a monorepo by **addition, not relocation**:

- The **root stays the tny harness tree** (`src/`, `tests/`, `docs/`,
  `Makefile`, `nix/`, `sdk/`). Nothing that release, Pages, or packaging
  automation reads moves.
- **Each new app is a self-contained top-level directory** with its own
  `Makefile`, `src/`, `tests/`, and `docs/` contract (including its own
  `docs/adr/` sequence). First sibling: [`tnytty/`](../../tnytty/docs/README.md).
- **`third_party/` at the root is shared.** Vendored libraries are pinned
  once, for every app; an app never re-vendors a library the root
  already carries.
- **Quality gates are shared.** Root `.clang-format`, `.clang-tidy`,
  `ruff.toml`, and `.shellcheckrc` apply repo-wide; `make quality`
  format-checks every tracked `*.c`/`*.h` (its `git ls-files` scope
  already includes sibling apps). Each app owns its stricter
  compile-time gates (`warn-strict`, tests) in its own Makefile.
- **The root Makefile delegates**: `make tnytty`, `make tnytty-test`
  build and test the sibling without entangling the harness's object
  lists, and CI runs sibling apps as separate jobs in `ci.yml`.
- **The nix flake stays harness-only for now.** `nix/source.nix`
  filesets do not include sibling apps; packaging tnytty in the flake is
  a later, separate change (tracked in tnytty's implementation plan).

If the Pages/packaging constraints are ever lifted (repo-settings
migration, major release), moving the harness under `tny/` gets a new
ADR; this one records why it has not happened yet.

## Consequences

- `AGENTS.md` documents the monorepo layout; sibling apps carry their
  own contract and are not governed by the harness's product invariants
  (size budget, backend rules) — only by the shared quality gates and
  the shared principles: C11, tiny, fast, docs before code.
- A change inside `tnytty/` never rebuilds or repackages the harness;
  release automation is untouched.
- Cross-app code sharing beyond `third_party/` (e.g. tny's `src/util/`)
  requires promotion into a shared library with its own contract, not
  `#include "../.."` reach-ins.
