# Language and runtime

## Decision: C11 with private C++20 ownership modules

[ADR 0114](adr/0114-private-cpp20-ownership-boundaries.md) authorizes a
limited migration for parser buffers/documents, runtime events/async
tools, and runner/job resources. It supersedes the old blanket C++ ban,
not the C ABI, platform support or reliability/performance gates. Keep
untouched application/transport/OS code, third-party libraries and
`tnytty` in C11. No public C++ ABI, Boost, UI framework or global allocator
replacement is introduced. Runtime size and dependencies are measured,
not inferred from the language.
[ADR 0115](adr/0115-owned-stream-decoding-and-failure-boundaries.md) covers
the parser owners, [ADR 0116](adr/0116-runtime-event-and-async-ownership.md)
the owned runtime events and custom-tool registration/async-call lifetimes,
and [ADR 0117](adr/0117-allocation-free-provider-oom-settlement.md) the
allocation-free provider settlement those owners rely on.
[ADR 0118](adr/0118-runner-and-job-resource-ownership.md) authorizes runner
and durable-job resource aggregates; platform process operations stay in C.
[ADR 0126](adr/0126-checkpoint-context-ownership.md) adds checked checkpoint
serialization and context recovery, with scoped temporary secret cleanup.
[ADR 0133](adr/0133-owned-subagent-launch-snapshots.md) adds private sub-agent
launch snapshots with failure-atomic replacement and full secret-block wiping.
Private facades
expose opaque owners and synchronous borrowed views, never standard-library
types.

## Compiler and link

| Item | Choice |
| --- | --- |
| Standard | C11 and scoped private C++20, `-Wall -Wextra -Werror`, no VLAs in new code |
| Debug | ASan/UBSan on the unit-test binary |
| Release | `-Os -ffunction-sections -fdata-sections`, strip, `--gc-sections` / `-dead_strip` |
| libc | macOS: libSystem (cannot static-link). Linux publish: **musl static** |
| TLS | macOS: Security.framework. Linux: **system OpenSSL** (`libssl.so.3` / `.so.1.1`), `dlopen`'d at first TLS use ([adr/0007](adr/0007-linux-tls-system-openssl.md)). Never link or vendor OpenSSL; musl static has no https |
| Threads | One event loop, native runner isolation, bounded independent file workers; no provider-host thread |
| Exceptions / RTTI | C++ allocation failures are caught at private C boundaries; RTTI is not needed |

## Library bill of materials

Vendor by source file, not by package manager graphs.

| Need | Library | Why |
| --- | --- | --- |
| JSON | [yyjson](https://github.com/ibireme/yyjson) | Fast, one `.c` |
| HTTP/1.1 + SSE | BSD sockets + [picohttpparser](https://github.com/h2o/picohttpparser) + ~200 LOC SSE | Drain the chunked body after `[DONE]`. Also accept `data: DONE` |
| Speech playback | Optional host `afplay`, `ffplay`, `mpv` or `mpg123` | External process only; no decoder library; [ADR 0071](adr/0071-ephemeral-host-audio-playback.md) |
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

Pin third-party versions in `third_party/*/VERSION`: yyjson, picohttpparser and greatest.

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


## Current optimization priorities

[ADR 0150](adr/0150-agent-first-harness-and-measured-footprint.md) removes
numeric artifact ceilings. Prefer readable, explicit ownership and
maintainable extension boundaries over byte-saving tricks. Measure stripped
size and runtime dependencies; do not fail a product gate on a byte maximum.
Latency, throughput, memory, fault recovery and ABI gates remain mandatory
and independently measured.

Implementation guide: [Extending the private C++ ownership layer](cpp-ownership.md).
