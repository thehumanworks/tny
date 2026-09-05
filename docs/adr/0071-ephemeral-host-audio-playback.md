# 0071 — Ephemeral host audio playback without a decoder dependency

Date: 2026-09-05
Status: accepted

## Context

Speech is a user interaction, so generating a named MP3 is the wrong default.
Bundling an MP3 decoder and audio driver would also cost binary size and add
platform maintenance. macOS TLS initialization makes fork-only subprocess
startup unsafe (ADR 0053's documented SecureTransport limitation).

## Decision

- Keep synthesis provider-independent and isolate host playback in
  `src/util/audio.c`, within ADR 0017's existing host OS seam. Use `afplay` on
  macOS, then `ffplay`, `mpv`, `mpg123` from PATH. No new linked/vendored audio
  dependency and no user-defined shell command interpolation.
- Validate the bounded response before playback. Create a private seekable
  temporary descriptor, immediately unlink its pathname, then write audio.
  Spawn the player with fixed argv and stdin bound to that descriptor.
  macOS `afplay /dev/fd/0` can read this input. Use `posix_spawn`, not fork
  after TLS. Suppress player stdout/stderr; expose a bounded tny diagnostic.
- Wait synchronously through `tny_poll`; success means the player exited
  successfully. Cooperative cancellation kills/reaps the player and returns
  130. A five-minute deadline bounds playback. No named audio artifact survives
  even process death after unlink. SIGKILL is inherently outside cooperative
  child cleanup; the anonymous input disappears when its last reader closes.
- `--output-file` explicitly selects export without playback. Write a private
  sibling temporary file and rename it only after successful completion;
  failure preserves the prior file. This is an intentional retained artifact.
- Playback targets the tny host, not the workspace SSH host or a remote
  renderer. Windows and wasm cleanly refuse playback; export uses the shared
  HTTP/filesystem service (browser CORS/virtual-filesystem constraints apply).
  No fourth platform seam or browser-to-C callback is introduced.
- The runtime lends blocking speech a flag-only cancellation probe. It must
  not re-enter the backend from a network/player wait. Other tools' execution
  and the detached runner ownership contract remain unchanged.

## Verification

The fake player verifies that stdin's inode has zero links, consumes exact
response bytes, and exercises nonzero exit and cancellation/reaping. HTTP
failures never launch it. Live macOS `afplay` succeeded with `cove` using the
existing ChatGPT account. Linux playback is covered with a fixture player;
real Linux audio hardware and browser export require their own environment.
Size and gate results are recorded with the implementation's final checks.

## Final measurements (2026-09-05)

Baseline: clean commit `df0560b`, built in a detached worktree on the same
macOS arm64 host. Both builds use the repository release Makefile and strip
step. The Linux build uses the installed Zig 0.16.0 C compiler targeting
`x86_64-linux-gnu`; its ELF was verified to contain neither a static symbol
table nor debug sections, then passed `make size-check`.

| Measurement | Baseline | Speech |
| --- | ---: | ---: |
| Stripped macOS binary | 850,224 bytes | 850,544 bytes |
| Stripped Linux x86_64 dynamic binary | not measured | 900,056 bytes (limit 1,048,576) |
| `--version` process wall time, median of 100 | 3.980 ms | 3.967 ms |
| Mock `ask-stdin`, median of 5 | 252.9 ms | 261.4 ms |
| Mock TUI first output, median of 5 | 121.9 ms | 117.7 ms |

The latency comparisons use `tests/bench/bench_ttft.py --iters 5
--rpc-delay 100`, with fixture speech credentials available and no live model
calls. These small samples include scheduler/process noise and do not support
an inference-time speedup claim. Mach-O segment padding absorbs most of the
added code/data, so file-size growth is not a measure of implementation size.

Final verification: `make test` (446 unit tests, 40 integration groups,
including 10 speech fixture tests), `make quality`, and `make leaks` passed.
The user's global Mise tool-profile override was removed from the test
process environment, using installed tool binaries directly so shims could
not reapply it; no workstation configuration was changed. The live account
verified default playback, a `gpt-5.6-luna` Codex turn invoking `speak`, and
explicit MP3 export (24 kHz mono, inspected by macOS `afinfo`, then removed).
Linux runtime/audio hardware, Windows execution and wasm execution were not
available locally; wasm export and unavailable-playback coverage is wired
into CI. No publication or installed-binary replacement is part of this change.
