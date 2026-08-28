# 0039 — First-party quality gates: `make quality` and a fast CI job

Date: 2026-08-27
Status: accepted

## Context

Until now compilation was the only static-analysis layer: `-Wall -Wextra
-Werror` on first-party code, ASan/UBSan on the debug suite, mutation tests,
and a five-platform build matrix. There was no formatter, no lint target, and
no analyzer configuration, so style and defect classes that the compiler does
not see (leak-on-failure realloc, use-after-free on error paths, unquoted
shell expansions) were only caught by review. A read-only tooling audit
(2026-08-27) recommended clang-format, clang-tidy, GCC `-fanalyzer`, a
stricter warning set, Ruff, ShellCheck/shfmt, actionlint, and a JS check,
enforced through one authoritative make target. Its first full run found and
fixed three real memory bugs (`ssh_connect` used a freed cwd in its error
message, `tool_abs_path` formatted a freed alias, `tools_execute` read an
uninitialized struct on early failure) plus eight leak-on-OOM realloc sites.

## Decision

`make quality` is the single authoritative aggregate for the current host:

- `format-check` — clang-format (`.clang-format`), `ruff format --check`
  (`ruff.toml`), and `shfmt -d -i 4 -ci -sr` over first-party C, Python, and
  shell. Adopting clang-format was a one-time mechanical reformat (~5k
  lines); the config keeps the existing 4-space/attached-brace/short-guard
  style, but multi-statement one-line blocks (`{ a; b; }`) are expanded —
  clang-format cannot represent them.
- `tidy` — clang-tidy (`.clang-tidy`): `clang-analyzer-*` plus selected
  `bugprone-*`, `cert-*`, `performance-*`, `portability-*`,
  `WarningsAsErrors: '*'`. Checks that flag deliberate design (command
  processors, struct padding, `(enum)0` sentinels) are disabled with
  reasons in the config.
- `warn-strict` — `-fsyntax-only` with `-Wpedantic -Wformat=2 -Wshadow
  -Wstrict-prototypes -Wmissing-prototypes -Wundef -Wwrite-strings -Wvla`;
  CI runs it with both gcc and clang so Linux-only code gets both compilers'
  diagnostics.
- `analyze` — GCC `-fanalyzer` (path-sensitive leaks, use-after-free,
  fd/stream misuse), included by `make quality` on Linux and skipped with an
  explicit message on non-Linux developer hosts; complementary to clang-tidy.
- `lint-py` — `ruff check` (rules E4/E7/E9/F/B/I). `sdk/schema/generated/`
  is excluded: it must stay byte-identical to the generator's output.
- `lint-sh` — ShellCheck (`.shellcheckrc` documents the two disabled
  info-level codes) over every tracked script.
- `lint-workflows` — actionlint over `.github/workflows/`.
- `lint-js` — `node --check` syntax gate over first-party `.js`/`.mjs`
  (vendored xterm.js and built wasm artifacts exempt). ESLint or
  `tsc --checkJs` remains a possible deepening; not adopted yet to keep the
  quality job dependency-free beyond Node itself.

CI runs a dedicated fast `quality` job on `ubuntu-24.04` before the platform
matrix, with pinned tool versions (clang-format 21.1.2, clang-tidy 22.1.8,
Ruff 0.14.0 via pipx; shfmt and actionlint via `go install`). Pinning
matters: formatter output changes between major versions.

The final required `ci` aggregate depends on the `quality` job as well as all
platform, wasm, TSan, and fuzz jobs, so static-analysis failures cannot be
masked by otherwise green builds.

Scope is first-party only: tracked C/header and shell inputs are discovered
from Git, while `third_party/`, frozen ABI fixtures, generated artifacts, and
built `docs/assets/wasm` stay exempt. `src/net/net_wasm.c` remains in the C
checks, but its EM_JS/EM_ASYNC_JS region is marked `clang-format off` because
clang-format tokenizes JavaScript `=>`/`===` as C operators and otherwise
produces invalid generated JavaScript; the wasm build and JS syntax gates
verify that embedded region instead.

## Consequences

- New code must pass `make quality`; run `make format` to auto-fix style.
- Local runs without LLVM tools can use the pinned wrappers, e.g.
  `make quality CLANG_FORMAT='uvx clang-format@21.1.2'
  CLANG_TIDY='uvx clang-tidy@22.1.8' RUFF='uvx ruff@0.14.0'`.
- A clang-tidy or `-fanalyzer` false positive is silenced in the config file
  (with a reason) or with a targeted `NOLINT`, never by loosening
  `WarningsAsErrors`.
- Future candidates deliberately deferred: `abidiff` (libabigail) against
  the previous release for libtny ABI drift; cppcheck; ESLint.
