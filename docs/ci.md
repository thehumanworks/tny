# CI

GitHub Actions builds the stripped `tny` binary on every pull request and on
`main`. Artifacts are named `tny-<os>-<arch>` and uploaded from the `ci`
workflow (`.github/workflows/ci.yml`).

## Matrix

| Artifact | Runner | Notes |
| --- | --- | --- |
| `tny-linux-x86_64` | `ubuntu-24.04` | glibc, ASan unit tests + fixtures |
| `tny-linux-aarch64` | `ubuntu-24.04-arm` | glibc, same tests |
| `tny-linux-x86_64-musl` | `ubuntu-24.04` + Alpine 3.21 | **static** musl (publish shape) |
| `tny-linux-aarch64-musl` | `ubuntu-24.04-arm` + Alpine 3.21 | **static** musl |
| `tny-darwin-arm64` | `macos-15` | Apple Silicon only |
| `tny-windows-x86_64.exe` | `windows-2025` + MSYS2 `MSYS` | POSIX via `msys-2.0.dll` |

The Pages workflow (`.github/workflows/pages.yml`) is separate and only
rebuilds the static site.

## Darwin is Metal / Apple Silicon, not Intel

macOS CI **must** be arm64. The darwin job runs on `macos-15` (M1) and
exits if `uname -m` is not `arm64`.

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
```
