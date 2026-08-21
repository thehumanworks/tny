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

## Consequences

- The native OpenAI-compatible loop and `tny acp` can view workspace
  images without a new dependency (hand-rolled base64 already existed).
- Session JSON stores data URLs for attached images, same as `--image`.
- TUI Ctrl-V paste writes a temp file and reuses this path.
