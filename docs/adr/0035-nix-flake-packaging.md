# 0035 — Nix packaging is a first-party flake that builds from source

Date: 2026-08-27
Status: accepted

## Context

Nix users had no supported way to consume tny. The obvious workarounds all
fail on something tny does deliberately:

- Fetching a release tarball into a `fetchurl` derivation ships a glibc-linked
  binary that no Nix store path vouches for, and the release matrix does not
  cover every system a flake claims.
- `make install PREFIX=$out` alone produces a binary whose first HTTPS request
  fails on NixOS. [ADR 0007](0007-linux-tls-system-openssl.md) `dlopen`s
  `libssl.so.3` at first TLS use precisely so nothing is on the link line —
  which is also why no store path ends up in the binary and why nothing on a
  Nix system is on a default library search path.
- [ADR 0014](0014-build-time-version-from-git.md) derives the version from
  `git describe`. A flake source tree has no `.git`, so an unprepared build
  reports `0.0.0-unknown`.

## Decision

Ship `flake.nix` plus `nix/*.nix` in this repository and build from source.

- `packages.tny` runs `make install`: the stripped binary and the pure-Python
  extension host under `lib/tny/`, which the `bin/../lib/tny` branch of
  `src/core/extensions.c` already looks for. `packages.libtny` runs
  `make install-lib` for the ABI-0 header, shared library and pkg-config file.
- **Linux TLS resolves through RUNPATH, added in `postFixup`.** glibc resolves
  a `dlopen` against the calling object's RUNPATH, so putting OpenSSL's store
  path there is enough and keeps the link line empty. It must happen after
  the fixup hooks, because `patchelf --shrink-rpath` drops any entry that no
  `DT_NEEDED` justifies — which is every entry, by design. macOS needs
  nothing: the SecureTransport shim uses an absolute framework path.
- **The version comes from the flake revision**, passed through the
  `TNY_VERSION` override ADR 0014 already documents for git-less builds. It
  is what `git describe --tags --always --dirty` prints for a tree with no
  reachable tag. Packaging a tag stays explicit:
  `pkgs.tny.override { version = "0.2.1"; }`.
- **The default binary is wrapped** with `python3` on PATH (`execlp("python3")`
  in the extension host) and a CA-bundle default, using `makeBinaryWrapper`
  rather than a shell wrapper. `packages.tny-unwrapped` skips it.
- **The size budget is enforced in `checkPhase`** (`make size-check`), so a Nix
  build cannot quietly exceed docs/size-and-speed.md.
- `checks.tests` runs the entire `make test` suite hermetically, so
  `nix flake check` is a real gate rather than a build smoke test.
- Systems are `x86_64-linux`, `aarch64-linux`, `aarch64-darwin`. Intel Mac is
  not a product target ([ADR 0006](0006-ci-build-targets.md)) and nixpkgs has
  dropped it.

Host agents stay external, as everywhere else: `cursor-sdk-bridge`, `codex`
and ACP agents are not inputs of this package.

## Measurements

Linux x86_64, this flake against nixpkgs 26.11, `hyperfine -N`, 300 runs after
20 warmups:

| Metric | value | budget |
| --- | ---: | ---: |
| stripped `tny` from `nix build` | 604,880 B | 1,572,864 B |
| `tny --version`, wrapped (default) | 0.73 ms ± 0.12 | < 5 ms |
| `tny --version`, `tny-unwrapped` | 0.42 ms ± 0.10 | < 5 ms |

The wrapper costs ~0.3 ms, an eighth of the startup budget. A shell wrapper
would have cost several times that, which is why the binary wrapper is not
optional-looking politeness.

## Consequences

- `nix run github:thehumanworks/tny`, `nix profile install`, an overlay, a
  flake input, or plain `nix-build` all work, and all build the same tree CI
  builds.
- A no-license repository has no nixpkgs license attribute. `meta.license` is
  left unset rather than set to `lib.licenses.unfree`, which would force every
  consumer of a first-party flake to pass `allowUnfree`. Redistribution is
  governed by `LICENSE-METADATA.json`; [nix.md](../nix.md) says so.
- No musl-static package. `STATIC=1` cannot `dlopen`, so https would fail on a
  package whose whole purpose is talking to providers.
- Two integration tests had to stop assuming their environment: `test_tui.py`
  skips the `git describe` cross-check when git is absent, and `test_https.py`
  scrubs `NIX_SSL_CERT_FILE` (a Nix-built OpenSSL prefers it over
  `SSL_CERT_FILE`, and `nix-build` points it at a nonexistent file).
- `nix develop` sets an OpenSSL RUNPATH through `NIX_LDFLAGS` but deliberately
  does **not** export `LD_LIBRARY_PATH`: a store OpenSSL there is picked up by
  every process started from the shell and drags a second glibc into host
  binaries. The consequence is that ASan intercepts `dlopen`, so glibc consults
  libasan's RUNPATH rather than the test binary's, and the TLS assertions in
  `make test` need `LD_LIBRARY_PATH` set for that one command. The hermetic
  path — `nix flake check` — sets it inside the sandbox, where there is no
  host to disturb.
- The wasm build is not packaged. `make wasm` needs emsdk, which is a separate
  toolchain concern; the flake covers native only.
