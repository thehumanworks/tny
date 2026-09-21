# CI

GitHub Actions builds the stripped `tny` binary on every pull request and on
`main`. Artifacts are named `tny-<os>-<arch>` and uploaded from the `ci`
workflow (`.github/workflows/ci.yml`).

## Local build cleanup

`make clean` removes `build/`, `dist/`, and root-level `build-*` directories
(for example, `build-provider/` and `build-sdk-final/`). These ignored directories
hold disposable binaries, intermediate files, test snapshots, and test reports.
Save any reports you need before cleaning, and wait for builds and tests in
these directories to finish. Cleanup does not detect active users.

With a custom `BUILD` value, such as `make clean BUILD=build-focus`, cleanup
removes only that directory and `dist/`. The extra-directory sweep applies
only when `BUILD` is exactly `build` (the default). It skips regular files and
symlinks matching `build-*`, and never searches nested or sibling-app trees.
Use `make tnytty-clean` separately for the terminal app.
See [ADR 0135](adr/0135-clean-build-variants.md).

## Matrix

| Artifact | Runner | Notes |
| --- | --- | --- |
| `tny-linux-x86_64` | `ubuntu-24.04` | glibc, the whole `make test` natively: ASan unit tests + every integration fixture |
| `tny-linux-aarch64` | `ubuntu-24.04-arm` | glibc, ASan unit tests, libtny fault/fuzz/ownership checks, size and package |
| `tny-linux-x86_64-musl` | `ubuntu-24.04` + Alpine 3.21 | **static** musl; unit tests + smoke |
| `tny-linux-aarch64-musl` | `ubuntu-24.04-arm` + Alpine 3.21 | **static** musl; unit tests + smoke |
| `tny-darwin-arm64` | `macos-15` | Apple Silicon only; ASan unit tests, shell workflows, libtny fault/fuzz/ownership checks, ImageMagick 7 conversion tests, size and package |
| `tny-wasm` (`tny.js`+`tny.wasm`, `tny-web.mjs`+`.wasm`) | `ubuntu-24.04` + emsdk 6.0.8 | the SAME openai/codex-profile mock suites with `TNY=build/wasm/tny`, measured wasm artifact size, and a headless-Chromium page smoke ([ADR 0017](adr/0017-wasm-browser-parity.md)) |

GitHub Actions builds and releases Linux and macOS native artifacts only
([ADR 0137](adr/0137-linux-macos-ci-and-optional-nix.md)). Windows jobs and
release assets are retired. Existing MSYS source/build seams remain for local
experimentation; this is not a deletion of platform code.

GCC native release LTO uses `-flto=auto`; Clang retains `-flto`. Local build
contract tests still check the optional MSYS flags without starting a Windows
runner or publishing a Windows artifact.

The Pages workflow also builds `tny-web.mjs` with emsdk and publishes it
under `assets/wasm/` — the landing terminal is the CI-tested artifact.

Every glibc/Darwin build lane also runs the sibling `tnytty` app's tests
and strict warnings from its own Makefile (docs/adr/0045). Size is reported,
not gated by a harness byte ceiling ([ADR 0150](adr/0150-agent-first-harness-and-measured-footprint.md)).

The Linux x86_64 lane runs the full integration suite. Linux aarch64 and
Darwin arm64 retain unit, fault, ownership and packaging checks; they no longer
have an additional hermetic full-suite CI lane. The hosted macOS full suite
previously took over two hours, so this change does not silently move it into
a short native job. The macOS lane now explicitly requires ImageMagick 7 and
runs the conversion suite, preserving coverage formerly supplied by Nix.

Runs on `main` are never cancelled by a newer push: the `ci` and `sdk`
workflows only cancel superseded pull-request runs. Both must succeed on the
same commit before automatic release.

`make test` covers native Responses and Chat streaming and tool calls, env-only named profiles, removed selectors and settings, no vendor executable discovery, and native Codex/Grok OAuth and compatible wires using local mocks. No paid/live inference is required. Library and SDK tests preserve lifecycle, events, cancellation, custom tools, and capability validation.

Nix is an **optional developer** workflow, not a GitHub Actions job or release
gate. `nix flake check` remains available locally for `x86_64-linux`,
`aarch64-linux` and `aarch64-darwin`; it builds `packages.tny`,
`packages.libtny` and `checks.tests`. The flake, dev shell, source filters and
local checks are retained. See [nix.md](nix.md) and
[ADR 0137](adr/0137-linux-macos-ci-and-optional-nix.md).

The Linux glibc and Darwin jobs also stage active ABI-1 `libtny` plus the
frozen ABI-0.8 compatibility library/header/pkg-config identity as
`libtny-<os>-<arch>`. MSYS2, musl-static, and wasm do not publish a public
shared-library artifact ([ADR 0037](adr/0037-libtny-abi-1.md)).

The dedicated SDK workflow (`.github/workflows/sdk.yml`) runs the Python and
Node version/platform matrices, cross-language conformance, clean package
installs, and native dependency inspection. Its aggregate `sdk` job fails
unless the packaging contract and every Python and Node matrix entry succeed;
use that single terminal status for branch protection.

SDK fixture builds include both `lib-shared` and `release`: ACP usage tests
launch the `tny` executable as their owning-runtime MCP bridge. The
`test-sdk-python` and `test-sdk-typescript` targets build both prerequisites.
The wasm lane also checks that ACP clients return an explicit unsupported
platform error. The fuzz/mutation lane has a 20-minute job budget for its
instrumented builds and complete mutation suites; individual fuzz runs retain
their existing time and iteration bounds.

Tagged release jobs also package those supported shared-library installs as
`libtny-<os>-<arch>.tar.gz`. Each archive contains the public header,
versioned library and linker name, pkg-config metadata, exact export manifests,
libtny documentation, explicit license metadata, and a deterministic per-file
SHA-256 manifest. The release-level `SHA256SUMS` covers both CLI and libtny
archives. SDK builds must consume one of these immutable inputs or an
explicitly supplied local install and record its artifact hash in their
conformance report.

The Pages workflow (`.github/workflows/pages.yml`) is separate. GitHub
Pages for this repo deploys from the branch (`main:/docs`, legacy build),
so the workflow rebuilds the static site from `site/` and commits the
output into `docs/` on `main`. Generated HTML and assets in `docs/` are a
published mirror of `site/` — edit `site/` and `scripts/site_build.py`,
never the generated files in `docs/`.

## Toolchain (`mise install`)

Every tool the gates shell out to is pinned twice, in lockstep
([ADR 0061](adr/0061-toolchain-and-leak-gates.md)): `.mise.toml` for
developers and the `quality` job in `ci.yml` for CI. One command gets a
machine to parity, and `make quality` then needs no flags:

```sh
mise install
make quality
make leaks
```

| Tool | Pin | mise backend |
| --- | --- | --- |
| clang-format | 23.1.0 | `pipx:` (the PyPI wheel CI installs; LLVM is not in the registry as a versioned pair) |
| clang-tidy | 22.1.8 | `pipx:` |
| ruff | 0.16.6 | `aqua:astral-sh/ruff` |
| shellcheck | 0.11.0 | `aqua:koalaman/shellcheck` |
| shfmt | 3.14.0 | `aqua:mvdan/sh` |
| actionlint | 1.7.12 | `aqua:rhysd/actionlint` |
| python | 3.14 | core |
| node | 26 | core |

The `pipx:` entries are driven through `uv`, which `.mise.toml` pins as
their prerequisite; no `experimental` setting is required.
`tests/integration/test_toolchain_pins.py` fails the suite if `.mise.toml`
and `ci.yml` drift apart.

Without mise, the per-invocation fallback still works:

```sh
make quality CLANG_FORMAT='uvx clang-format@23.1.0' \
             CLANG_TIDY='uvx clang-tidy@22.1.8' RUFF='uvx ruff@0.16.6'
```

The Nix dev shell carries the same tools at the channel's versions
([nix.md](nix.md)); `mise install` is the version-exact path.

## Leak checks

`make leaks` is the memory gate. ASan/UBSan is the default test build and no
leak checker can see through it, so the target rebuilds the same sources with
`SANITIZE=0` into `build/leakcheck/` and runs the unit binary plus the CLI
smoke (`--version`, `--help`, `ask --help`, `doctor --json`) under the host's
checker:

| Host | Checker | Coverage |
| --- | --- | --- |
| Linux | `valgrind --leak-check=full --error-exitcode=1 --child-silent-after-fork=yes --errors-for-leak-kinds=definite,indirect --suppressions=tests/valgrind.supp` | whole unit binary in one run, then the smoke |
| macOS | `/usr/bin/leaks --atExit` | suite by suite; valgrind has no arm64 Darwin port |
| other | honest skip, exit 0 | Linux CI is the gate |

`make valgrind` is the explicit Linux-only target (an error elsewhere);
`make leaks-docker` runs the valgrind flavour from a non-Linux host in a
throwaway `ubuntu:24.04` container (`LEAK_DOCKER_IMAGE=` overrides it). Note
that the container mounts the working copy, so the tree has to be inside your
Docker file-sharing roots — colima and Docker Desktop share `$HOME`, not
`/tmp`, by default.

macOS runs suite by suite and skips `mcp_suite`, `session_bg_suite`, `ssh_suite` and `runner_suite` (its control-channel
tests fork a terminal child, ADR 0058): `leaks --atExit` installs an
exit hook that stops the process for analysis and `fork(2)` copies it into
every child, so a suite that spawns a helper deadlocks, and
MallocStackLogging's banner corrupts the stdout those tests read back. The
Linux `valgrind` job covers all six, which is why it is the gate.

Two valgrind flags beyond the obvious ones earn their place. Several suites
fork, and a child that exits mid-test reports the parent's still-live heap as
lost — only the parent's report is the truth, so
`--child-silent-after-fork=yes`. And `possibly lost` here is glibc's per-thread
stack and DTV for threads alive at exit, never a first-party leak, so
`--errors-for-leak-kinds=definite,indirect` decides the exit code.

A `valgrind` job on `ubuntu-24.04` runs `make valgrind` on every PR.
`tests/valgrind.supp` suppresses only the dynamic loader and the dlopen'd
system OpenSSL that `src/net/stream.c` deliberately never closes; first-party
leaks are never suppressed.

## Releases (mise / `github:` backend)

Pushing a `v*` tag runs `.github/workflows/release.yml`: the same matrix,
packaged as `tny-<os>-<arch>[-musl].tar.gz` for Linux/macOS, plus `libtny1-*` / `libtny0-compat-*`, SDK wheels, npm
tarballs, conformance reports, and `SHA256SUMS`, published as a GitHub
release. The publish job flattens `dist/sdk/*` to the asset root before the
globs run. Each CLI archive also carries the pure-Python extension host under
`lib/tny/` and the sourceable Bash/Zsh workflow library under `share/tny/`;
Python and the shells themselves are never bundled. The
version is not hardcoded anywhere: make derives it from `git describe`
(docs/adr/0014), release jobs pass `TNY_VERSION=${tag#v}` explicitly
(shallow checkouts and the Alpine container have no tags), and the
`version` job fails the release if the built binary does not report the
pushed tag.

Releases are what make `mise x github:thehumanworks/tny -- tny --version`
work — mise resolves versions from GitHub releases and autodetects the
asset from the os/arch/libc words in its name, so keep the triple naming.
While the repo is private, mise needs `GITHUB_TOKEN` (or
`MISE_GITHUB_TOKEN`) set to list and download releases.

Release flow: merge to `main`. Nothing else — the `auto-release` workflow
(`.github/workflows/auto-release.yml`, docs/adr/0085) tags and publishes
every merge once the `ci` and `sdk` workflows are both green on that
commit. Whichever of the two completes last does the work: it checks the
other gate through the Actions API, runs `scripts/next_release_version.py`,
pushes the annotated tag as `github-actions[bot]`, and dispatches
`release.yml` on the tag ref (a tag pushed with `GITHUB_TOKEN` never fires
`on: push: tags`, so the dispatch is the trigger, not a fallback). The
`release` run then builds, packages, certifies the SDK artifacts and
publishes; it does not repeat the test suite, which the two gates already
ran on that commit ([ADR 0110](adr/0110-one-suite-per-platform.md)), and
the auto-release job fails loudly if that run does not start.

The version comes from the commit messages since the newest stable
`vX.Y.Z` tag reachable from the commit (Conventional Commits prefixes,
scanned on every commit in the range, so a PR's branch commits count):

| Since the last tag                                | Bump                         |
| ------------------------------------------------- | ---------------------------- |
| `feat:` / `feat(scope):`                          | minor                        |
| `type!:` or a `BREAKING CHANGE:` footer           | minor before 1.0, then major |
| anything else (`fix:`, `ci:`, `docs:`, no prefix) | patch                        |

Opt out of releasing one merge with `[skip release]` (or `[no release]`) in
the squashed commit or in any commit the merge introduces; the next merge
releases both. A commit whose gates finish after a newer commit has already
been released is skipped, so tags never move backwards. Pre-release tags
(`v1.2.3-rc.1`) are ignored as bases and are still cut by hand.
Merges that touch `src/` are followed by the Pages bot's `[skip ci]` mirror
commit, so the release tag normally sits one commit below the tip of `main`.

Manual paths still work: `git tag v<version> && git push origin v<version>`
starts `release.yml` directly (a tag on a `[skip ci]` commit will not — pick
the merge commit; the release does not test, so tag only commits whose gates
are green), and "Run workflow" on `auto-release` cuts a release from
the newest commit with green gates, optionally forcing the bump kind. If a
tag exists without a release, dispatch it on the tag ref:
`gh workflow run release.yml --ref v<version>`.

## Darwin is Metal / Apple Silicon, not Intel

macOS CI **must** be arm64. The darwin job runs on `macos-15` (M1) and
exits if `uname -m` is not `arm64`.
Tagged libtny and SDK artifacts set `MACOSX_DEPLOYMENT_TARGET=13.0`; release
inspection must reject a dylib, addon, or wheel that raises that minimum.
Linux glibc SDK artifacts similarly fail compatibility inspection if they
require symbols newer than glibc 2.34; musl remains unsupported for libtny.

Do **not** add `macos-15-intel`, `macos-26-intel`, `macos-*-large`, or any
other x86_64 Mac runner. Intel Mac is not a product target.

## Retired automation

No Windows runner, Windows release archive, or Nix CI job is scheduled.
Historical releases remain unchanged. Existing MSYS code and optional local
Nix commands are retained; neither is part of release eligibility. Branch
protection should require the remaining `ci` and `sdk` checks, not a retired
Nix check.

## Size reporting

There is no product binary-size ceiling
([ADR 0150](adr/0150-agent-first-harness-and-measured-footprint.md)). CI
and `make size-check` report stripped bytes and runtime dependencies.
The compatibility target still rejects missing, empty, non-executable or
unrecognized-header artifacts. `wasm-size-check` reports glue and module bytes
and validates the module header. Neither target enforces a byte maximum.

## Local

```sh
mise install           # the pinned toolchain (docs/adr/0061)
make quality           # formatting, lint, tidy, strict warnings; GCC analyzer on Linux
make leaks             # valgrind (Linux) / leaks (macOS) over the unit suite + smoke
make test-shell-workflows # the workflow scheduler under both Bash and Zsh
make test              # unit (ASan) + integration fixtures
make test-abi          # ABI baseline, old consumers, exports, artifacts
make test-sdks         # Python and TypeScript SDK + conformance adapters
make size-check        # report stripped size (no product byte ceiling)
make STATIC=1 release  # musl static, on Alpine or a musl toolchain
make pack TRIPLE=linux-x86_64
nix flake check        # the same suite, hermetically (docs/nix.md)
```

## Benchmarks

Benchmarks are never part of `make test` — shared runners are too noisy for a
timing or pass-rate gate. They are run deliberately, and their numbers are
recorded in the ADR that motivated them.

`tests/bench/bench_ttft.py` measures time-to-first-token against the strict
openai mock ([ADR 0004](adr/0004-ttft.md)).

`tests/bench/bench_tools.py` is the three-arm A/B of the native tool profiles
`all`, `terminal+edit` and `terminal` ([ADR 0062](adr/0062-tool-profiles.md)),
whose result is recorded in the Measurement section of
[ADR 0057](adr/0057-shell-first-native-loop.md). Every run copies one frozen
fixture from `tests/bench/fixtures/tools/<task>/` into a fresh scratch
directory, feeds its `task.md` to `tny ask -B --json --stdin`, blocks on
`tny session ID --wait --json`, and scores the scratch with the fixture's
`check.sh` (exit 0 = pass). The session document supplies steps, tool calls,
token usage, repair loops and edit-method drift, so no second provider call is
needed. The harness shadows `PATH` with the binary under test, because the
shell profiles tell the model to reach for `tny edit`.

```sh
python3 tests/bench/bench_tools.py --dry-run           # list the frozen task set
python3 tests/bench/bench_tools.py --verify-fixtures   # red before, green after
python3 tests/bench/bench_tools.py --mock --tasks fix-py-sum-range
python3 tests/bench/bench_tools.py --provider aiproxy --effort high --runs 1 \
    --max-steps 40 --timeout 600                       # the live pilot; needs a key
```

`--mock` needs no key: it scripts `tests/integration/mock_openai.py` to issue
one `terminal` call that runs the fixture's own `solution.sh`, so the whole
pipeline is exercised offline. That is what the CI smoke
`tests/integration/test_bench_tools.py` runs — one task in each of the three
arms, plus `--dry-run`, `--verify-fixtures`, and the unknown-task error — and
it is picked up automatically by `tests/integration/run.sh`, so `make test` and
`nix flake check` both cover it. Live arms need a provider key and are never
run in CI.

Each fixture directory holds the workspace files plus four pieces of
bookkeeping that are never copied into the scratch a model sees: `task.md`
(the prompt), `check.sh` (the programmatic check), `family`, and `solution.sh`
(the reference solution, which both proves the check is satisfiable and drives
the `--mock` trajectory). Checks are deterministic and offline; the C fixtures
need only `cc`.

### Runtime ownership and provider OOM (ADR 0116, ADR 0117)

`make test-runtime-ownership` links the real runtime unit suite with the C++
allocator fault lane. It checks retained payloads, reserve settlement without
allocation, transactional recovery and independent async leases.
`make test-libtny-fault` also builds `build/lib-fault/provider-faults`, the
the real native OpenAI HTTP backend linked against the fully instrumented object
graph; `tests/integration/test_libtny_faults.py` runs its named regressions and
the whole-turn provider allocation sweeps. `make test-runtime-mutation`
(`tests/mutation/runtime_critical.py`) compiles private mutant copies of the
runtime, owner and provider sources and requires behavioral kills; production
sources remain untouched. `make test-libtny-fault-sanitize` also runs the C and
C++ custom-tool worker fixtures, including completion-time OOM, through the
instrumented library. Native CI and Nix include the runtime target; Linux
`make test-libtny-tsan` remains the concurrency detector gate.

### Runner and job ownership (ADR 0118)

`make test-runner-ownership` compiles `tests/fixtures/runner_ownership.cpp`,
which binds the real `runner.cpp`/`jobs.cpp` sources with real descriptor,
pipe and advisory-lock boundaries, an allocator-instrumented `alloc.c` and
syscall-faulting copies of the unchanged C host seams. It checks descriptor
transfer and reuse, writer ownership at final save and socket removal, partial
job transactions, failed launches, cleanup holds, checkpoint consumption and
cancellation authority. `make test-runner-mutation`
(`tests/mutation/runner_critical.py`) compiles private mutants of those
sources and requires behavioral kills. Both run in the native CI suite, the
musl unit lanes (ownership fixture) and optional local Nix checks.
