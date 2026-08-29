# 13 — Exercise Nix on aarch64-linux

Medium-low. From the repository-wide test audit. Independent.

`flake.nix` declares `aarch64-linux` alongside `x86_64-linux` and
`aarch64-darwin`. `.github/workflows/nix.yml` only runs `ubuntu-24.04`
(x86_64) and `macos-15` (arm64 Darwin). Native arm64 Linux CI covers
the Makefile path, not Nix wrapping, RUNPATH, closure, or sandbox
behavior on that system.

## Work

- Add an `ubuntu-24.04-arm` entry to the nix workflow matrix that runs
  the same `nix flake check` + binary smoke + revision assertion as
  x86_64 Linux.
- Keep the “no Intel Mac” guard; do not add `x86_64-darwin`.
- Update `docs/ci.md` / `docs/nix.md` so the documented Nix matrix
  matches the workflow.

## Acceptance

- A Nix-only breakage on aarch64-linux (wrap, RUNPATH, `checks.tests`
  sandbox) fails `nix.yml` the same way it would on x86_64-linux.
- `docs/ci.md` no longer implies Nix CI is only x86_64 Linux + Darwin.
