# Nix

tny ships a first-party flake. It builds the same tree CI builds — there is no
prebuilt binary to trust, no `fetchurl` of a release tarball, and no vendored
toolchain. Design rationale: [ADR 0035](adr/0035-nix-flake-packaging.md).

Systems: `x86_64-linux`, `aarch64-linux`, `aarch64-darwin`. Intel Mac is not a
tny target ([ADR 0006](adr/0006-ci-build-targets.md)).

## Run it

```sh
nix run github:thehumanworks/tny                      # interactive TUI
nix run github:thehumanworks/tny -- ask "explain this repo"
nix profile install github:thehumanworks/tny          # put tny on PATH
```

Pin a tag the usual way: `github:thehumanworks/tny/v0.2.1`.

## Use it from your own flake

```nix
{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  inputs.tny.url = "github:thehumanworks/tny";

  outputs = { nixpkgs, tny, ... }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        packages = [ tny.packages.${system}.tny ];
      };
    };
}
```

Or take the overlay, which adds `tny` and `libtny` to your package set:

```nix
pkgs = import nixpkgs {
  inherit system;
  overlays = [ tny.overlays.default ];
};
# then: pkgs.tny, pkgs.libtny
```

The overlay is the form to use with `environment.systemPackages` on NixOS or
`home.packages` with home-manager.

## Outputs

| Output | What |
| --- | --- |
| `packages.<system>.tny` (`default`) | CLI/TUI binary plus the Python extension host under `lib/tny/` |
| `packages.<system>.tny-unwrapped` | the same binary with no wrapper — see [Wrapping](#wrapping) |
| `packages.<system>.libtny` | the active ABI-1 embedding library, header and `libtny.pc` ([libtny.md](libtny.md)); frozen ABI-0 compatibility remains in release artifacts |
| `apps.<system>.tny` (`default`) | `nix run` entry point |
| `overlays.default` | `tny` and `libtny` for any nixpkgs instance |
| `devShells.<system>.default` | toolchain for `make`, `make test`, `make bench` |
| `checks.<system>` | the package builds, plus the whole `make test` suite |
| `formatter.<system>` | `nix fmt` for the Nix files |

## Without flakes

```sh
nix-build -A tny                       # build/tny + the extension host
nix-build -A libtny                    # the embedding library
nix-build -A tny --argstr version 0.2.1
nix-shell                              # the dev shell
```

`default.nix` and `shell.nix` call the same `nix/*.nix` files the flake does,
against `<nixpkgs>`.

## What is and is not in the closure

Host agents stay external processes, exactly as they do everywhere else in tny.
`cursor-sdk-bridge`, `codex` and ACP agents are **not** dependencies of this
package; put them on PATH yourself (for example in the same `mkShell`
`packages` list, or via `--agent CMD`). `tny doctor` reports which ones it can
see.

`python3` is not required for tny itself. The extension host
([extensions.md](extensions.md)) `execlp`s `python3` from PATH, and the
default package supplies one through its wrapper; without it, extensions
report unavailable and everything else works.

The wasm build is not packaged — `make wasm` needs emsdk
([ADR 0017](adr/0017-wasm-browser-parity.md)).

## Version

[ADR 0014](adr/0014-build-time-version-from-git.md) makes the git tag the only
source of truth and commits no version string. A flake source tree has no
`.git`, so the flake passes its own revision through the `TNY_VERSION`
override that ADR already documents for git-less builds — `tny --version`
prints the short revision, the same thing `git describe --tags --always` prints
for a tree with no reachable tag.

To stamp a release version instead:

```nix
pkgs.tny.override { version = "0.2.1"; }
```

## TLS

Linux tny `dlopen`s the system OpenSSL at first TLS use and links nothing
([ADR 0007](adr/0007-linux-tls-system-openssl.md)). Nothing is on a default
library search path under Nix, so the package writes OpenSSL's store path into
the binary's RUNPATH, which is what glibc consults for a `dlopen` made by the
executable itself. macOS needs nothing — the SecureTransport shim uses an
absolute framework path.

Two consequences worth knowing:

- **A Nix-built OpenSSL reads `NIX_SSL_CERT_FILE` in preference to
  `SSL_CERT_FILE`.** If you are pointing tny at a private CA and it is ignored,
  that is why. Set `NIX_SSL_CERT_FILE`, or unset it and set `SSL_CERT_FILE`.
- The default package also sets `SSL_CERT_FILE` to nixpkgs' `cacert` bundle
  *if you have not set it*, so tny works in a bare container with no
  `/etc/ssl/certs`.

There is no musl-static package. A static build cannot `dlopen`, so https
would fail outright.

## Wrapping

`packages.tny` is wrapped with `makeBinaryWrapper` — a compiled wrapper, not a
shell script — to add `python3` to PATH and default the CA bundle. Measured on
Linux x86_64, that costs about 0.3 ms of the 5 ms `--version` budget
(0.73 ms wrapped vs 0.42 ms unwrapped, `hyperfine -N`, 300 runs). If you want
the binary to inherit your environment untouched, use
`packages.<system>.tny-unwrapped`.

## Developing

```sh
nix develop                # cc, make, python3, node, hyperfine, openssl, git
nix flake check            # builds every package and runs the whole make test
nix build .#checks.x86_64-linux.tests --print-build-logs
nix fmt                    # format the Nix files
direnv allow               # .envrc enters the dev shell on cd
```

`nix flake check` is the hermetic way to run the suite: it runs the same
`make test` CI runs, in a sandbox, with `LD_LIBRARY_PATH` pointed at OpenSSL.

Two things the suite normally borrows from the host are spelled out for the
builder, whose PATH holds only its own inputs. `tests/integration/test_tui.py`
reads `ps` to prove the pre-warm spawned exactly one host, so the derivation
carries `procps` on Linux and `darwin.ps` on macOS. And cc-wrapper appends its
link flags to any call it cannot prove is compile-only — `-fsyntax-only` among
them — so the installed-header check in `tests/integration/test_libtny.py` gets
`-L` paths it never links, which clang reports as unused arguments and `-Werror`
turns into a failure; the derivation and the dev shell answer that with
`-Wno-unused-command-line-argument` wherever the compiler is clang. The dev
shell keeps your PATH, so it needs the flag but not `ps`.

The dev shell deliberately does **not** export `LD_LIBRARY_PATH` — a store
OpenSSL on that path is inherited by every process you start from the shell and
pulls a second glibc into host binaries. It sets an OpenSSL RUNPATH through
`NIX_LDFLAGS` instead, which covers `build/tny`. It cannot cover the ASan test
binary, because ASan intercepts `dlopen` and glibc then consults libasan's
RUNPATH rather than the binary's, so on a host with no loadable system OpenSSL
`make test` needs the search path for that one command:

```sh
LD_LIBRARY_PATH="$(nix eval --raw nixpkgs#openssl.out)/lib" make test
```

## License

`LICENSE-METADATA.json` records `LicenseRef-UNLICENSED`: this repository grants
no project license, and all rights are reserved unless the copyright holder
says otherwise. That has no nixpkgs `lib.licenses` attribute, and marking the
package `unfree` would force every consumer of a first-party flake to pass
`allowUnfree`, so `meta.license` is left unset. Redistributing builds of tny —
including into a public binary cache — is governed by that file and by
`THIRD_PARTY_NOTICES.md`, not by the absence of a `meta` attribute.
