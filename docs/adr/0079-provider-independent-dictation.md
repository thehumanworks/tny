# 0079 — Provider-independent dictation into the composer

Status: Accepted

## Context

Users need microphone speech transcribed into prompts without changing the
agent provider. TTS already reuses Codex account credentials independently;
dictation needs that same separation in the input direction. C11, small
binaries, lazy optional host audio, one event loop, and wasm behavior remain
constraints.

## Decision

- Add a provider-neutral `core/dictation` lifecycle shared by the CLI and TUI:
  recording → transcribing → complete, with explicit finish/cancel/result.
- Put provider authentication, multipart construction, HTTP parsing, and
  transport lifecycle behind `dictation_provider.h`. The first adapter is
  Codex, using ChatGPT subscription credentials and `/backend-api/transcribe`.
- Pin the wire contract to OpenAI Codex **rust-v0.105.0**,
  `codex-rs/tui/src/voice.rs:transcribe_bytes`. That release sends one PCM WAV
  multipart `file` and reads JSON `text`; the newer Codex CLI release used
  for other tny features no longer includes that implementation. Live account
  verification is required rather than inferring current endpoint support.
- Keep capture in the existing host OS seam, using `posix_spawn` and an audio
  pipe: FFmpeg/AVFoundation on macOS; arecord/ALSA or FFmpeg/PulseAudio on
  Linux. No linked decoder, recording framework, new thread, or startup probe.
- The common service owns bounded audio and transcript memory. Capture uses
  mono 24-kHz PCM16; file input accepts complete PCM16 WAVs. Limit clips to
  1–300 seconds / 25 MiB and transcripts to 64 KiB. Reject malformed,
  truncated, empty, or terminal-control-bearing output before insertion.
- `/dictate` or Ctrl-R starts capture. Enter/Ctrl-R finishes it; Esc/Ctrl-C
  cancels it. Insert validated text at the caret and preserve the existing
  draft. Require a subsequent Enter to submit normally to the chat provider.
- Keep the UI responsive through recorder/response fds on `tny_poll`; the
  existing HTTP connect/upload seam remains synchronous and bounded. A new
  approval/clarification cancels dictation before taking the keyboard.
- `tny dictate` emits text or one JSON result on stdout and progress on stderr.
  It does not load the selected conversation profile. `--stt-provider` and
  `TNY_STT_PROVIDER` are independent of chat configuration. Device selectors
  are literal argv, and only the existing trusted Codex gateway override can
  redirect the subscription bearer.
- No recording files, agent microphone tool, or new public embedding ABI.
  Windows/wasm reject microphone capture cleanly while file transcription
  uses their existing HTTP seam (browser CORS rules apply).

## Consequences

Users can dictate with their ChatGPT account while Grok or another provider
runs the agent loop. New STT providers only implement an adapter. Host audio
tools remain optional, with platform/device permission checked by the actual
capture attempt. Cancellation discards audio/text and stops only the owned
recorder/HTTP operation; it does not cancel an existing agent turn.

The endpoint is an account backend, so compatibility is isolated in one file
and verified with the user's account. No assumption is made about an OpenAI
API key supporting that route. No new JS or platform seam is introduced.

## Verification

- Unit tests cover WAV bounds/chunk integrity, text validation, provider
  isolation, composer insertion/capacity, and split-safe shortcut decoding.
- HTTP and fake-recorder fixtures cover multipart bytes, account refresh,
  secret-safe errors, chunked UTF-8, malformed/oversized/truncated responses,
  cancellation and recorder ownership. PTY screen assertions cover editable
  draft insertion, explicit submission, and separate Grok/Codex credentials.
- Live account test on 2026-09-08: a locally generated known phrase was
  transcribed exactly by ChatGPT with `--provider grok` selected. Neither a
  Grok request nor a conversation session was required for that transcription.
- `make test`: **458 unit tests**, **43 integration groups**, including
  **18 dictation tests**, passed on macOS arm64.
- `make quality` passed; GCC `-fanalyzer` is explicitly skipped on Darwin
  and remains a Linux CI gate. `make leaks` reported zero leaked bytes.
- Targeted dictation mutation pass: **2 valid mutants killed**, **2 rejected
  by the compiler**, **0 survivors**; original source and builds restored.
- Stripped macOS arm64 binary: **900,896 bytes** (baseline **883,904**).
  The Linux aarch64-musl static cross-build is **966,248 bytes**, below the
  **1,048,576-byte** gate. This is cross-build proof, not Linux runtime proof.
- A live microphone run captured/uploaded audio but returned no speech text;
  a later direct recorder probe returned `Audio device not found`. A spoken
  hardware test remains unverified. The test restored the host's original
  mute and volume settings.
- Windows and wasm runtimes were not available locally. The wasm CI job
  runs the file fixtures and checks unavailable capture without a microphone.
