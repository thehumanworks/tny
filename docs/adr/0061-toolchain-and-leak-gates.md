# 0061 — One `mise install` for the toolchain; a leak gate with valgrind and `leaks`

Date: 2026-09-02
Status: accepted (extends 0039 quality gates; relates to 0035 nix packaging,
0045 monorepo, 0057 shell-first native loop)

## Context

ADR 0039 made `make quality` the authoritative first-party gate, but it left
the tools themselves to the host. `.github/workflows/ci.yml` pins them —
clang-format 21.1.2, clang-tidy 22.1.8, ruff 0.14.0, shfmt 3.13.1, actionlint
1.7.12 — through `pipx` and `go install`; a developer machine got whatever
Homebrew, apt, or nixpkgs happened to hold. clang-format's output changes
between majors, so a locally clean tree could still fail CI on formatting
alone, and AGENTS.md answered that with a per-invocation incantation:

```sh
make quality CLANG_FORMAT='uvx clang-format@21.1.2' CLANG_TIDY='uvx clang-tidy@22.1.8'
```

That works and is worth keeping as a fallback, but it is not a toolchain: it
covers three of the eight tools the gates shell out to, and it has to be
retyped every run.

The second gap is memory. The unit suite builds with ASan/UBSan, which catches
overflows and use-after-free but reports no leaks at all on Darwin (LSan has no
arm64 macOS support) and is not enabled for leak detection on Linux either.
GCC's `-fanalyzer` finds leaks it can prove statically along one path; nothing
in the repo observed the actual heap of an actual run. tny is a long-lived TUI
process that allocates per event, so "we never checked" is not a position.

valgrind is the obvious answer and has no aarch64-Darwin port, which is the
platform the maintainer develops on. macOS ships `/usr/bin/leaks`, which does
the same job through a different mechanism, with a different failure mode.

## Decision

**Pin the whole quality toolchain in a project `.mise.toml`, and add a
platform-aware leak gate (`make leaks`) that CI enforces on Linux with
valgrind.**

1. **`.mise.toml` pins eight tools**: clang-format 21.1.2 and clang-tidy
   22.1.8 through the `pipx:` backend (the same PyPI wheels CI installs — LLVM
   is not in the mise registry as a versioned pair, and `uv` is pinned as the
   backend's prerequisite), ruff 0.14.0, shellcheck 0.11.0, shfmt 3.13.1 and
   actionlint 1.7.12 through `aqua:`, plus python 3.12 and node 22 for the
   fixtures and site tests. No `experimental` setting is required.
   `mise install` puts all of them on PATH, so the Makefile's existing
   `CLANG_FORMAT ?=` style defaults resolve to the pinned binaries and
   `make quality` needs **no flags**. The uvx fallback stays documented in
   `docs/ci.md` for machines without mise.

2. **The pins are checked, not trusted.**
   `tests/integration/test_toolchain_pins.py` reads `.mise.toml` and
   `.github/workflows/ci.yml` and fails the suite when the two disagree, when
   a tool is dropped from either, or when the docs stop mentioning
   `mise install`. Two pin sites are unavoidable — CI runners cannot bootstrap
   from a mise config without adding a third-party action to the trust
   boundary — so the lockstep is enforced instead of assumed.

3. **`make leaks` is the memory gate.** It rebuilds the same sources with
   `SANITIZE=0` into `build/leakcheck/` (no leak checker can see through
   ASan's replaced malloc) and runs the unit binary plus the CLI smoke
   — `--version`, `--help`, `ask --help`, `doctor --json` — under the host's
   checker. `scripts/leakcheck.sh` is the driver:
   - **Linux**: `valgrind --leak-check=full --error-exitcode=1
     --child-silent-after-fork=yes --errors-for-leak-kinds=definite,indirect
     --suppressions=tests/valgrind.supp`, the whole unit binary in one run.
     The two extra flags are load-bearing: several suites fork, and a child
     that exits mid-test reports the parent's still-live heap as lost (the
     first run "found" a 296-byte `mcp_warm_start` leak that way — the parent
     frees it, the forked child simply has no thread holding the pointer); and
     `possibly lost` is glibc's per-thread stack/DTV for threads alive at
     exit. Definite and indirect losses decide the exit code.
   - **macOS**: `/usr/bin/leaks --atExit`, **suite by suite**.
   - **anything else**: an honest skip, exit 0.

   `make valgrind` is the explicit Linux-only target (a clean error, exit 2,
   elsewhere) and `make leaks-docker` runs the valgrind flavour from a Mac in a
   throwaway `ubuntu:24.04` container.

4. **macOS skips five suites, and says so.** `leaks --atExit` installs an exit
   hook that stops the process for analysis, and `fork(2)` copies that hook
   into every child: the first spawned helper to exit is stopped forever and
   the run deadlocks. MallocStackLogging also writes a banner into every
   instrumented process, which corrupts the stdout `ssh_suite` reads back from
   its helpers. `cursor_suite`, `cursor_sdk_suite`, `mcp_suite`,
   `session_bg_suite` and `ssh_suite` are therefore skipped on Darwin — the
   Linux valgrind job covers all five, which is why that job, not the macOS
   convenience path, is the gate. The suite list is derived from
   `RUN_SUITE(...)` in `tests/test_main.c`, so a new suite joins the gate
   automatically.

5. **`tests/valgrind.supp` suppresses only foreign allocations**: the dynamic
   loader, and the system OpenSSL that `src/net/stream.c` dlopen's at first TLS
   use and deliberately never closes. A first-party leak is never suppressed.

6. **CI gains a `valgrind` job** on `ubuntu-24.04` running `make valgrind`.
   The `quality` job keeps its explicit `pipx`/`go install` pins rather than
   adopting a mise action: the version-exactness is what matters, and (2)
   enforces it without widening CI's dependency surface.

7. **Nix**: the dev shell gains the linters (clang-tools, ruff, shellcheck,
   shfmt, actionlint) at channel versions and valgrind on Linux. The leak gate
   is **not** a flake check — memcheck needs `ptrace` on a process it launches,
   Darwin sandboxes have no `/usr/bin/leaks`, and a valgrind-paced unit suite
   is roughly twenty times `checks.tests` — so `docs/nix.md` records that and
   points at the CI job.

### wasm behaviour

Neither gate touches the wasm build. `make leaks` and `make valgrind` are
**native-only, clean error / honest skip**: there is no valgrind or `leaks` for
a `.wasm` module, and `emcc` builds no `build/leakcheck` binary. Leak coverage
for wasm remains what it has always been — the same C, checked natively — and
the `wasm` CI job is unchanged. `.mise.toml` pins no emsdk; the wasm job keeps
its own pinned emsdk 6.0.8 (ADR 0017).

## Consequences

- One command (`mise install`) brings any macOS arm64 or Linux machine to the
  exact versions CI enforces, and `make quality` stops needing flags. The
  Makefile is unchanged apart from a comment: PATH does the work.
- A pin bump is now a two-file edit that the suite verifies, instead of a
  one-file edit that silently breaks everyone's local run.
- Leak regressions in first-party code fail a PR on Linux. macOS gets a fast
  local approximation with a documented hole (five process-spawning suites).
- `make leaks` compiles a third object set (`build/leakcheck/`, sanitizer-free)
  alongside `build/rel` and `build/dbg`. It is not part of `make test`; leak
  checking is opt-in locally and a separate CI job.
- The first full run found **zero first-party leaks**, and the numbers are the
  baseline the gate now protects: 18 of 23 suites plus four CLI smokes report
  `0 leaks for 0 total leaked bytes` under `leaks` on macOS 27/arm64, and on
  ubuntu-24.04/valgrind 3.22 the whole unit binary reports `definitely lost: 0`
  and `indirectly lost: 0` (288 bytes possibly lost — one pthread DTV) with the
  four smokes at `All heap blocks were freed`. Removing
  `tests/valgrind.supp` changes none of those definite/indirect numbers; it only
  moves ~612 KB of OpenSSL tables between `still reachable` and `suppressed`.
- mise becomes a documented-but-optional dependency. Nothing requires it: CI
  installs tools itself, Nix has its own shell, and the uvx fallback still
  works.
