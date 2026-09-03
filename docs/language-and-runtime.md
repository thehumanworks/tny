# Language and runtime

## Decision: C11

Pick **C11** (GNU C11 on GCC/Clang is fine). Do not use C++ as the implementation language.

fx is already a **6.4 MiB (macOS) / 11.1 MiB (static Linux) Zig** native binary with zero package deps. Beating that with Go, Rust, or C++ (libstdc++ / exceptions / RTTI / iostreams) is unlikely. C++/musl hello-world is already ~75× C/musl. C keeps the binary a thin layer over libc + a few vendored files.

C++ is allowed only as an *optional* generated stub if a future tool cannot emit C. Prefer [nanopb](https://github.com/nanopb/nanopb) and hand-rolled Connect so that never happens.

Zig was considered and rejected: the user constrained the choice to C or C++, and C is the smaller, faster runtime of those two.

## Compiler and link

| Item | Choice |
| --- | --- |
| Standard | C11, `-Wall -Wextra -Werror`, no VLAs in new code |
| Debug | ASan/UBSan on the unit-test binary |
| Release | `-Os -ffunction-sections -fdata-sections`, strip, `--gc-sections` / `-dead_strip` |
| libc | macOS: libSystem (cannot static-link). Linux publish: **musl static** |
| TLS | macOS: Security.framework. Linux: **system OpenSSL** (`libssl.so.3` / `.so.1.1`), `dlopen`'d at first TLS use ([adr/0007](adr/0007-linux-tls-system-openssl.md)). Never link or vendor OpenSSL; musl static has no https |
| Threads | One event loop. TUI prewarm uses one bounded connection thread; Cursor may lend its loopback callback server to one bounded pump thread during a blocking store RPC. Custom tools remain owner-thread-only |
| Exceptions / RTTI | N/A (C) |

## Library bill of materials

Vendor by source file, not by package manager graphs.

| Need | Library | Why |
| --- | --- | --- |
| JSON | [yyjson](https://github.com/ibireme/yyjson) | Fast, one `.c` |
| HTTP/1.1 + SSE | BSD sockets + [picohttpparser](https://github.com/h2o/picohttpparser) + ~200 LOC SSE | Drain the chunked body after `[DONE]`. Also accept `data: DONE` |
| WebSocket | [wslay](https://github.com/tatsuhiro-t/wslay) | Framing only; tny owns TCP/TLS + the handshake |
| Protobuf | No runtime for Cursor requests; pinned `.proto` files plus deterministic contract metadata. A minimal bounded decoder handles `SdkErrorDetails` Any payloads | JSON is the forward-compatible sdk.v1 interchange; no C++ protobuf runtime |
| Connect | Hand-rolled HTTP/1.1 framing plus bounded loopback callback server | Unary + server streams outbound; authenticated custom-tool/store RPCs inbound; classic gRPC will not work |
| TUI | Raw ANSI + termios + UTF-8 width | No ncurses, notcurses, termbox |
| Tests | [greatest.h](https://github.com/silentbicycle/greatest) | One header. Golden files in `testdata/` |
| MCP TOML | `src/util/toml.c` (~400 LOC subset) | Codex/grok `[mcp_servers.*]` import only ([ADR 0051](adr/0052-mcp-import-from-harnesses.md)); not a general TOML library |

Do **not** take: libcurl, OpenSSL, libuv, Boost, nlohmann/json, protobuf C++, grpc, libwebsockets, cJSON, ICU, gtest. Cross-compile C with `zig cc` if needed; do not write Zig. ("Take" means vendor or link; `dlopen`ing the platform's TLS library — Security.framework, system libssl — is the intended alternative, [adr/0007](adr/0007-linux-tls-system-openssl.md).)

## Build

POSIX `Makefile` first. Targets: `tny`, `tny-test`, `lib-shared`,
`install-lib`, `size-check`, `pack`. ABI 0's shared-library platform and
packaging contract is documented in [libtny.md](libtny.md) and
[ADR 0023](adr/0023-libtny-embedding-abi.md).
macOS **Apple Silicon** and Linux (x86_64 + aarch64, glibc and musl static)
are v1. Windows CI builds via MSYS2 `MSYS` (POSIX runtime, `msys-2.0.dll`);
native Win32 is later. Intel Mac is not a CI or publish target
([adr/0006](adr/0006-ci-build-targets.md), [ci.md](ci.md)).

Pin third-party versions in `third_party/*/VERSION`. Cursor's release protos
and generated `contract.json` live under
`third_party/cursor-sdk-bridge/v1.0.30/`; neither is hand-edited.

Nix builds go through the same Makefile ([nix.md](nix.md),
[ADR 0035](adr/0035-nix-flake-packaging.md)) and add no library to the bill of
materials: OpenSSL stays `dlopen`'d and unlinked, reaching the binary as a
RUNPATH entry rather than a `-lssl`. `nix/*.nix` is packaging, not a build
system — if a make target changes, the flake follows it, never forks it.

## Layout (when code starts)

See [architecture.md](architecture.md). Keep every translation unit under ~500 lines. One backend directory per protocol. Shared net code has no knowledge of agents.
