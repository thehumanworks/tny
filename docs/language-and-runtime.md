# Language and runtime

## Decision: C11 with private C++20 ownership

[ADR 0114](adr/0114-private-cpp-parser-ownership.md) authorizes C++20 only for
SSE/Connect accumulation, Chat/Responses event decoding and tool-call owners.
[ADR 0116](adr/0116-runtime-event-and-async-ownership.md) additionally authorizes
owned runtime events and custom-tool registration/async-call lifetimes.
Untouched code, vendored dependencies and tnytty remain C11. Public libtny
headers, layouts and exports remain C. Private facades expose opaque owners
and synchronous borrowed views, never standard-library types.

C++ allocation uses the existing tny allocator, with local exception
containment. No global operator new override, Boost, iostreams or new JSON
library is introduced. Runtime size and startup are measured rather than
inferred from the implementation language.

## Compiler and link

| Item | Choice |
| --- | --- |
| Standard | C11 / private C++20, `-Wall -Wextra -Werror`, no VLAs in new code |
| Debug | ASan/UBSan on the unit-test binary |
| Release | `-Os -ffunction-sections -fdata-sections`, strip, `--gc-sections` / `-dead_strip` |
| libc | macOS: libSystem (cannot static-link). Linux publish: **musl static** |
| TLS | macOS: Security.framework. Linux: **system OpenSSL** (`libssl.so.3` / `.so.1.1`), `dlopen`'d at first TLS use ([adr/0007](adr/0007-linux-tls-system-openssl.md)). Never link or vendor OpenSSL; musl static has no https |
| Threads | One event loop. TUI prewarm uses one bounded connection thread; Cursor may lend its loopback callback server to one bounded pump thread during a blocking store RPC. Custom tools remain owner-thread-only |
| Exceptions / RTTI | C++ allocation exceptions caught at private C boundaries; RTTI disabled |

## Library bill of materials

Vendor by source file, not by package manager graphs.

| Need | Library | Why |
| --- | --- | --- |
| JSON | [yyjson](https://github.com/ibireme/yyjson) | Fast, one `.c` |
| HTTP/1.1 + SSE | BSD sockets + [picohttpparser](https://github.com/h2o/picohttpparser) + ~200 LOC SSE | Drain the chunked body after `[DONE]`. Also accept `data: DONE` |
| WebSocket | [wslay](https://github.com/tatsuhiro-t/wslay) | Framing only; tny owns TCP/TLS + the handshake |
| Protobuf | No runtime for Cursor requests; pinned `.proto` files plus deterministic contract metadata. A minimal bounded decoder handles `SdkErrorDetails` Any payloads | JSON is the forward-compatible sdk.v1 interchange; no C++ protobuf runtime |
| Connect | Hand-rolled HTTP/1.1 framing plus bounded loopback callback server | Unary + server streams outbound; authenticated custom-tool/store RPCs inbound; classic gRPC will not work |
| Speech playback | Optional host `afplay`, `ffplay`, `mpv` or `mpg123` | External process only; no decoder library; [ADR 0071](adr/0071-ephemeral-host-audio-playback.md) |
| TUI | Raw ANSI + termios + UTF-8 width | No ncurses, notcurses, termbox |
| Tests | [greatest.h](https://github.com/silentbicycle/greatest) | One header. Golden files in `testdata/` |
| MCP TOML | `src/util/toml.c` (~400 LOC subset) | Codex/grok `[mcp_servers.*]` import only ([ADR 0051](adr/0052-mcp-import-from-harnesses.md)); not a general TOML library |

Do **not** take: libcurl, OpenSSL, libuv, Boost, nlohmann/json, protobuf C++, grpc, libwebsockets, cJSON, ICU, gtest. Cross-compile C with `zig cc` if needed; do not write Zig. ("Take" means vendor or link; `dlopen`ing the platform's TLS library — Security.framework, system libssl — is the intended alternative, [adr/0007](adr/0007-linux-tls-system-openssl.md).)

## Build

`CC` compiles `.c` as C11; `CXX` compiles `.cpp` as C++20 and links mixed
artifacts. `CXXFLAGS` adds caller flags; lane optimization, sanitizers and
visibility apply to both languages. C++ uses explicit allocator calls, not
the C-only force-included allocation macros. `EMCXX=em++` handles wasm C++
objects and links with exception catching enabled. ABI0 is built unchanged
from its frozen C source archive. See [CI](ci.md) for parser safety targets.


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

## Optional Git worktrees

Git worktree mode invokes the installed `git` executable only when requested.
No Git library is linked or vendored. Native POSIX builds support local
worktrees; wasm reports a clean error. See [Worktrees](worktrees.md).

## Optional microphone capture

Dictation launches an external recorder only when requested: FFmpeg with
AVFoundation on macOS, arecord (ALSA) or FFmpeg (PulseAudio) on Linux. No
recording/decoding library or platform framework is linked into tny; PCM16
WAV framing is C11 in the shared service. Windows and wasm support remote
file transcription and cleanly reject microphone capture. See
[Dictation](dictation.md) and [ADR 0079](adr/0079-provider-independent-dictation.md).
