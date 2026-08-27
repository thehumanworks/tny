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
| `tny-darwin-arm64` | `macos-15` | Apple Silicon only; `make test` minus cursor mock |
| `tny-windows-x86_64.exe` | `windows-2025` + MSYS2 `MSYS` | POSIX via `msys-2.0.dll`; unit + smoke |
| `tny-wasm` (`tny.js`+`tny.wasm`, `tny-web.mjs`+`.wasm`) | `ubuntu-24.04` + emsdk 6.0.8 | the SAME openai/acp-ws/codex-attach mock suites with `TNY=build/wasm/tny`, `wasm-size-check`, and a headless-Chromium page smoke ([ADR 0017](adr/0017-wasm-browser-parity.md)) |

The Pages workflow also builds `tny-web.mjs` with emsdk and publishes it
under `assets/wasm/` — the landing terminal is the CI-tested artifact.

A separate `nix` workflow (`.github/workflows/nix.yml`) runs `nix flake check`
on `ubuntu-24.04` and `macos-15`: it builds `packages.tny` (whose `checkPhase`
is `make size-check`), `packages.libtny`, and `checks.tests` — the whole
`make test` suite in a sandbox — then smokes the built binary and asserts it
reports this commit's revision. See [nix.md](nix.md) and
[ADR 0035](adr/0035-nix-flake-packaging.md). It publishes no artifact; Nix
users build from source.

The Linux glibc and Darwin jobs also stage the experimental ABI-0 `libtny`
developer tree (`include/tny/tny.h`, shared library, and pkg-config metadata)
as `libtny-<os>-<arch>`. MSYS2, musl-static, and wasm do not publish a public
library artifact in ABI 0 ([ADR 0023](adr/0023-libtny-embedding-abi.md)).

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

## Releases (mise / `github:` backend)

Pushing a `v*` tag runs `.github/workflows/release.yml`: the same matrix,
packaged as `tny-<os>-<arch>[-musl].tar.gz` (Windows: `.zip` with
`msys-2.0.dll`) plus `SHA256SUMS`, published as a GitHub release. Each archive
also carries the pure-Python extension host under `lib/tny/`; Python itself is
never bundled. The
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
make test              # unit (ASan) + integration fixtures
make size-check        # fail if over the host budget
make STATIC=1 release  # musl static, on Alpine or a musl toolchain
make pack TRIPLE=linux-x86_64
nix flake check        # the same suite, hermetically (docs/nix.md)
```
