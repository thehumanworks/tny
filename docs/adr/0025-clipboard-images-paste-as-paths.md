# 0025 — Clipboard images paste as paths

Date: 2026-08-24
Status: accepted

## Context

Ctrl-V previously materialized clipboard pixels under `/tmp`, queued that file
as an image attachment, and inserted an `[Image #N]` placeholder. Host backends
such as Cursor and ACP do not carry image prompts. They rejected the turn before
its text was sent, while the queued attachment remained and made every later
send fail too; `/clear` only clears the terminal and could not repair that
provider state.

ADR 0008 deliberately makes image attachments native-loop content parts, but
clipboard paste is a shared composer action and must not assume the selected
provider has that capability.

## Decision

**Ctrl-V materializes a valid clipboard image under `/tmp` and inserts its
filesystem path as inline-code composer text.** The backticks prevent an
absolute path at column zero from being parsed as a slash command. Ctrl-V never
queues an image attachment. The prompt therefore follows the same
provider-neutral text path as typed input, and an agent that can inspect local
files may choose its own image-reading tool.

`/image PATH` and `tny ask --image PATH` remain the explicit attachment
interfaces for image-capable native-loop providers. Clipboard text fallback is
unchanged.

This supersedes only ADR 0008's consequence that TUI Ctrl-V reuses the native
image-attachment path. Its `image_url` and `read_image` decisions remain in
force.

## Consequences

- Cursor, Codex, and ACP can receive a pasted-image path without an unsupported
  image argument.
- Ctrl-V no longer adds an image count or `[Image #N]` placeholder to the TUI.
- Extracted clipboard files continue to live under `/tmp`, as before, so the
  agent can read the path after submission.
