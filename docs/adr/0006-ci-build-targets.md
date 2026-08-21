# 0006 — CI builds Linux (x86_64 + aarch64), Darwin Apple Silicon, and Windows

Date: 2026-08-21
Status: accepted

## Context

tny had no compile/test workflow — only GitHub Pages. The product size
budgets ([size-and-speed.md](../size-and-speed.md)) already say CI must fail
a PR that exceeds them, and the publish shape for Linux is **musl static**.
Users also need a Darwin arm64 binary (Apple Silicon / Metal) and a Windows
binary. Intel Mac is not a target.

The C sources are POSIX. A native Win32 port (CreateProcess, Winsock,
console API) is a large follow-up, not a CI prerequisite.

## Decision

**GitHub-hosted runners build every PR on this matrix:**

1. **Linux x86_64 and aarch64**, glibc, on `ubuntu-24.04` and
   `ubuntu-24.04-arm`. Full `make test`.
2. **Linux musl static** for both arches, via Alpine 3.21 in Docker on those
   same runners. This is the Linux publish artifact.
3. **Darwin arm64 only**, on `macos-15`. The job asserts `uname -m == arm64`
   and must never use `macos-*-intel` or `macos-*-large`.
4. **Windows x86_64** on `windows-2025` with MSYS2 `MSYS` (not MinGW/UCRT).
   That runtime provides `fork` / `poll` / `termios` so the existing C
   compiles. Ship `msys-2.0.dll` next to `tny.exe`.

Size gates: 1.5 MiB Linux, 1.8 MiB Darwin, 2.0 MiB Windows. Host binaries
stay external.

## Consequences

- PRs get downloadable artifacts named `tny-<os>-<arch>`.
- Adding an Intel Mac job is a product decision that needs a new ADR; the
  workflow comments say not to.
- A later native Win32 port can replace the MSYS job without changing the
  Linux/Darwin matrix.
- Alpine/Docker is an extra CI dependency; it is not a library linked into
  `tny`.
