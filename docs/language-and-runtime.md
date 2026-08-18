# Language and runtime

## Decision: C11

Pick **C11** (GNU C11 on GCC/Clang is fine). Do not use C++ as the implementation language.

fx is already a 7.8 MiB **Zig** native binary with almost no runtime. Beating that with Go, Rust (libstd + tokio), or C++ (libstdc++ / exceptions / RTTI / iostreams) is unlikely once TLS, JSON, and a TUI land. C keeps the binary a thin layer over libc + a few vendored files.

C++ is allowed only as an *optional* generated stub if a future tool cannot emit C. Prefer [nanopb](https://github.com/nanopb/nanopb) and hand-rolled Connect so that never happens.

Zig was considered and rejected: the user constrained the choice to C or C++, and C is the smaller, faster runtime of those two.

## Compiler and link

| Item | Choice |
| --- | --- |
| Standard | C11, `-Wall -Wextra -Werror`, no VLAs in new code |
| Debug | ASan/UBSan on the unit-test binary |
| Release | `-O2 -fno-plt`, strip, hidden visibility |
| libc | Dynamic system libc on macOS; musl static optional on Linux |
| TLS | Dynamic Secure Transport / system LibreSSL / OpenSSL. Do not statically link a TLS stack into the default binary |
| Threads | Avoid. One event loop. Extra threads only for a Connect callback server if Cursor custom tools require it |
| Exceptions / RTTI | N/A (C) |

## Library bill of materials

Vendor by source file, not by package manager graphs.

| Need | Library | Why |
| --- | --- | --- |
| JSON | [yyjson](https://github.com/ibireme/yyjson) | Fast, tiny, C, in-situ parse |
| HTTP/1.1 + TLS | Small custom client **or** dynamically linked libcurl | Connect unary/stream and OpenAI SSE. Prefer custom+system TLS if curl pulls extra size |
| WebSocket | [wslay](https://github.com/tatsuhiro-t/wslay) | Tiny frame codec; tny owns the TCP/TLS socket |
| Protobuf | [nanopb](https://github.com/nanopb/nanopb) | C, no C++ protobuf runtime |
| Connect | Hand-rolled (~150 LOC) | Bridge is HTTP/1.1 only; classic gRPC will not work |
| TUI | Raw ANSI + UTF-8 columns | No ncurses, no notcurses, no termbox |
| Tests | A 200-line `tny_test.h` | No Criterion/Check |

Do **not** take: libuv, Boost, nlohmann/json, protobuf C++, grpc, libwebsockets (heavy), cJSON (slower, messier), ICU.

## Build

POSIX `Makefile` first. Targets: `tny`, `tny-test`, `compile_commands` (optional bear). macOS and Linux are v1. Windows is later.

Pin third-party versions in `third_party/*/VERSION`. Generated nanopb output lives in `gen/` and is not hand-edited.

## Layout (when code starts)

See [architecture.md](architecture.md). Keep every translation unit under ~500 lines. One backend directory per protocol. Shared net code has no knowledge of agents.
