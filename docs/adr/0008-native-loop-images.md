# 0008 — Native-loop images are user-role content parts

Date: 2026-08-21
Status: accepted

## Context

Chat Completions accepts vision input as `content` parts
`{"type":"image_url","image_url":{"url":"data:image/png;base64,..."}}`.
Many OpenAI-compatible providers reject those parts on `role: "tool"`
messages (the tool result must stay a string). The old `vision` tool was a
stub that told the model to use `--image` / `/image` instead, so a model
that tried to open a referenced screenshot via `read_file` got binary
garbage.

## Decision

**`read_image` is a first-class native-loop tool.** It detects png/jpeg/gif/webp
from magic bytes, rejects files over 8 MiB, and returns a short text tool
result. The pixels are then injected as a follow-up **user** message with
`image_url` parts (same shape as `tny ask --image` and TUI paste).

`vision` remains an alias. `read_file` refuses recognized image files and
points at `read_image`. Host backends are unchanged: they already ignore or
reject `--image`.

`tny ask --image PATH` is repeatable up to 16 paths. The 17th flag is a
startup error (`tny: too many --image flags (max 16)`, exit 1) before any
image file is opened or a backend is connected. At that point the only owned
buffer is the accumulated prompt; `images[]` holds borrowed argv pointers.
The overflow return frees `prompt`, matching the `--output-schema` and
unknown-flag paths. The TUI `/image` queue is a separate cap of 8.

## Consequences

- The native OpenAI-compatible loop and `tny acp` can view workspace
  images without a new dependency (hand-rolled base64 already existed).
- Session JSON stores data URLs for attached images, same as `--image`.
- TUI Ctrl-V paste writes a temp file and reuses this path.
- `cmd_ask` overflow of `--image` does not leak the prompt buffer. The
  ASan/UBSan unit test `cmd_ask_image_overflow_frees_prompt` drives
  `cmd_ask` with a leading prompt token plus 17 `--image` flags
  (`make test-unit`). macOS ASan does not support `detect_leaks=1`; Linux
  ASan can run that option on the same test.
