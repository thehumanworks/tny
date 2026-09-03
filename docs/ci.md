# CI

GitHub Actions builds the stripped `tny` binary on every pull request and on
`main`. Artifacts are named `tny-<os>-<arch>` and uploaded from the `ci`
workflow (`.github/workflows/ci.yml`).

## Matrix

| Artifact | Runner | Notes |
| --- | --- | --- |
| `tny-linux-x86_64` | `ubuntu-24.04` | glibc, ASan unit tests + fixtures |
| `tny-linux-aarch64` | `ubuntu-24.04-arm` | glibc, same tests |
| `tny-linux-x86_64-musl` | `ubuntu-24.04` + Alpine 3.21 | **static** musl; unit tests + smoke |
| `tny-linux-aarch64-musl` | `ubuntu-24.04-arm` + Alpine 3.21 | **static** musl; unit tests + smoke |
| `tny-darwin-arm64` | `macos-15` | Apple Silicon only; deterministic sdk.v1 contract/unit tests run, while the spawned Python Cursor bridge fixture retains its documented Darwin-CI skip |
| `tny-windows-x86_64.exe` | `windows-2025` + MSYS2 `MSYS` | POSIX via `msys-2.0.dll`; unit + smoke |
| `tny-wasm` (`tny.js`+`tny.wasm`, `tny-web.mjs`+`.wasm`) | `ubuntu-24.04` + emsdk 6.0.8 | the SAME openai/acp-ws/codex-profile mock suites with `TNY=build/wasm/tny`, `wasm-size-check`, and a headless-Chromium page smoke ([ADR 0017](adr/0017-wasm-browser-parity.md)) |

The Pages workflow also builds `tny-web.mjs` with emsdk and publishes it
under `assets/wasm/` — the landing terminal is the CI-tested artifact.

`make quality` and `make test` verify the vendored Cursor v1.0.30 hashes and
contract counts before accepting the adapter. Native integration fixtures
cover all 27 outbound routes, the custom-tool and custom-store reverse RPCs,
local/cloud options, Create/Resume callback re-entry, Send/Observe recovery,
all three stream types, cancellation, management aliases/raw RPC, structured
errors, auth, secret non-leakage, and process cleanup. wasm asserts exact clean
unsupported errors for both conversational Cursor and `tny cursor` management
before any bridge or callback work. These are deterministic
protocol tests; CI has no `CURSOR_API_KEY` and makes no live Cursor Cloud claim.
The libtny/Python/TypeScript matrices also exercise Cursor provider creation,
normalized events, cancellation, custom tools, capabilities, and validation;
they do not expose the management RPC surface.

A separate `nix` workflow (`.github/workflows/nix.yml`) runs `nix flake check`
on `ubuntu-24.04` (`x86_64-linux`), `ubuntu-24.04-arm` (`aarch64-linux`), and
`macos-15` (`aarch64-darwin`). The three entries share one `check` job and the
same steps: each builds `packages.tny` (whose `checkPhase` is `make size-check`),
`packages.libtny`, and `checks.tests` — the whole `make test` suite in a
sandbox — then smokes the built binary, asserts it reports this commit's
revision, and `nix-instantiate`s `default.nix` / `shell.nix`. A wrap, RUNPATH,
or sandbox-only breakage on aarch64-linux fails this workflow the same way it
would on x86_64-linux. The Darwin entry still exits if `uname -m` is not
`arm64`; do not add `x86_64-darwin`. See [nix.md](nix.md) and
[ADR 0035](adr/0035-nix-flake-packaging.md). It publishes no artifact; Nix
users build from source.

The Linux glibc and Darwin jobs also stage active ABI-1 `libtny` plus the
frozen ABI-0.8 compatibility library/header/pkg-config identity as
`libtny-<os>-<arch>`. MSYS2, musl-static, and wasm do not publish a public
shared-library artifact ([ADR 0037](adr/0037-libtny-abi-1.md)).

The dedicated SDK workflow (`.github/workflows/sdk.yml`) runs the Python and
Node version/platform matrices, cross-language conformance, clean package
installs, and native dependency inspection. Its aggregate `sdk` job fails
unless the packaging contract and every Python and Node matrix entry succeed;
use that single terminal status for branch protection.

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
| clang-format | 21.1.2 | `pipx:` (the PyPI wheel CI installs; LLVM is not in the registry as a versioned pair) |
| clang-tidy | 22.1.8 | `pipx:` |
| ruff | 0.14.0 | `aqua:astral-sh/ruff` |
| shellcheck | 0.11.0 | `aqua:koalaman/shellcheck` |
| shfmt | 3.13.1 | `aqua:mvdan/sh` |
| actionlint | 1.7.12 | `aqua:rhysd/actionlint` |
| python | 3.12 | core |
| node | 22 | core |

The `pipx:` entries are driven through `uv`, which `.mise.toml` pins as
their prerequisite; no `experimental` setting is required.
`tests/integration/test_toolchain_pins.py` fails the suite if `.mise.toml`
and `ci.yml` drift apart.

Without mise, the per-invocation fallback still works:

```sh
make quality CLANG_FORMAT='uvx clang-format@21.1.2' \
             CLANG_TIDY='uvx clang-tidy@22.1.8' RUFF='uvx ruff@0.14.0'
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

macOS runs suite by suite and skips `cursor_suite`, `cursor_sdk_suite`,
`mcp_suite`, `session_bg_suite`, `ssh_suite` and `runner_suite` (its control-channel
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
packaged as `tny-<os>-<arch>[-musl].tar.gz` (Windows: `.zip` with
`msys-2.0.dll`), plus `libtny1-*` / `libtny0-compat-*`, SDK wheels, npm
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

Release flow: merge to `main`, then
`git tag v<version> && git push origin v<version>`. No version bump commit
is needed — the tag is the single source of truth.

## Darwin is Metal / Apple Silicon, not Intel

macOS CI **must** be arm64. The darwin job runs on `macos-15` (M1) and
exits if `uname -m` is not `arm64`.
Tagged libtny and SDK artifacts set `MACOSX_DEPLOYMENT_TARGET=13.0`; release
inspection must reject a dylib, addon, or wheel that raises that minimum.
Linux glibc SDK artifacts similarly fail compatibility inspection if they
require symbols newer than glibc 2.34; musl remains unsupported for libtny.

Do **not** add `macos-15-intel`, `macos-26-intel`, `macos-*-large`, or any
other x86_64 Mac runner. Intel Mac is not a product target.

## Windows

The sources are POSIX (`fork`, `poll`, `termios`, Unix sockets). Native
Win32 (MSVC / MinGW without a POSIX runtime) is still later.

Windows CI uses the MSYS2 **MSYS** environment so those APIs exist. The
artifact is `tny-windows-x86_64.exe` plus `msys-2.0.dll`. It is a real
Windows binary, not a cross-compiled stub; it is not a native Win32 port.

## Size gates

CI fails the job if the stripped binary exceeds the Must column in
[size-and-speed.md](size-and-speed.md):

| Target | Limit |
| --- | --- |
| Linux glibc and musl static | 1.5 MiB (1,572,864 B) |
| Darwin arm64 | 1.8 MiB (1,887,436 B) |
| Windows MSYS | 2.0 MiB (2,097,152 B) |

`make size-check` is the local equivalent. Override with `SIZE_MAX=`.

## Local

```sh
mise install           # the pinned toolchain (docs/adr/0061)
make quality           # formatting, lint, tidy, strict warnings; GCC analyzer on Linux
make leaks             # valgrind (Linux) / leaks (macOS) over the unit suite + smoke
make test-shell-workflows # the workflow scheduler under both Bash and Zsh
make test              # unit (ASan) + integration fixtures
make test-abi          # ABI baseline, old consumers, exports, artifacts
make test-sdks         # Python and TypeScript SDK + conformance adapters
make size-check        # fail if over the host budget
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
