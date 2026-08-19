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
| TLS | macOS: Security.framework. Linux: trimmed **mbedTLS** client (TLS 1.2+1.3). Never OpenSSL |
| Threads | Avoid. One event loop. Extra threads only for a Connect callback server if Cursor custom tools require it |
| Exceptions / RTTI | N/A (C) |

## Library bill of materials

Vendor by source file, not by package manager graphs.

| Need | Library | Why |
| --- | --- | --- |
| JSON | [yyjson](https://github.com/ibireme/yyjson) | Fast, one `.c` |
| HTTP/1.1 + SSE | BSD sockets + [picohttpparser](https://github.com/h2o/picohttpparser) + ~200 LOC SSE | Drain the chunked body after `[DONE]`. Also accept `data: DONE` |
| WebSocket | [wslay](https://github.com/tatsuhiro-t/wslay) | Framing only; tny owns TCP/TLS + the handshake |
| Protobuf | [nanopb](https://github.com/nanopb/nanopb) | C, no C++ protobuf runtime |
| Connect | Hand-rolled (~150 LOC) | HTTP/1.1 only; classic gRPC will not work |
| TUI | Raw ANSI + termios + UTF-8 width | No ncurses, notcurses, termbox |
| Tests | [greatest.h](https://github.com/silentbicycle/greatest) | One header. Golden files in `testdata/` |

Do **not** take: libcurl, OpenSSL, libuv, Boost, nlohmann/json, protobuf C++, grpc, libwebsockets, cJSON, ICU, gtest. Cross-compile C with `zig cc` if needed; do not write Zig.

## Build

POSIX `Makefile` first. Targets: `tny`, `tny-test`, `compile_commands` (optional bear). macOS and Linux are v1. Windows is later.

Pin third-party versions in `third_party/*/VERSION`. Generated nanopb output lives in `gen/` and is not hand-edited.

## Layout (when code starts)

See [architecture.md](architecture.md). Keep every translation unit under ~500 lines. One backend directory per protocol. Shared net code has no knowledge of agents.
