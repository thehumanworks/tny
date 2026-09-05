# 0073 — Image CLI and agent tools share one service

Date: 2026-09-05
Status: accepted

## Context

Image generation must be discoverable by both ordinary CLI users and native
agents, including the default shell tool profiles. A nested process would
lose explicit flag credentials, session permission grants and cooperative
cancellation, as described in ADR 0063.

## Decisions

1. Add `tny image generate` and `tny image edit` alongside the existing
   socket-bound `image attach`. Dispatch before chat runtime setup. Stdin
   holds the UTF-8 prompt (ADR 0064); flags hold options and local paths.
   `--image-provider` avoids conflating the image adapter with global chat
   `--provider`. `--model` after the image subcommand selects the image model.
   Require `--output-file`, repeat `--image` for edit references, expose a
   local-only `--check`, and preserve `--cwd` and ChatGPT credential globals.
2. Plain success prints the path; `--json` prints kind, operation, provider,
   model, path, actual MIME type and byte count. Never print base64 image
   content or the request prompt. Preserve standard 0/1/2/130 exit meanings.
   Typed tools return the same metadata with workspace-resolved paths.
3. Expose `image_generate` / `image_edit` in the full native tool profile,
   gated by local ChatGPT credentials. Advertise explicit instructions in
   the shared native preamble, including shell equivalents and a reminder to
   inspect output with `read_image`. Shell profiles retain their existing
   schema. Cursor/ACP host loops remain host-owned; standalone CLI help is
   available to those agents without injecting native tools.
4. Extend the narrow first-party terminal interception to generation/editing
   with piped or quoted-heredoc prompts. Reuse the exact CLI option parser,
   serialize arguments, then invoke the same tool adapter and image service.
   Keep unsupported shell grammar, background commands and `--check` with
   normal terminal execution. Never launch an additional agent turn.
5. Give each operation its own sensitive permission identity. The grant
   scope is a JSON-escaped tuple of provider, canonical output, and ordered
   reference paths. This makes every uploaded reference visible and prevents
   grants for one set being silently reused for another. Do not classify
   either operation as read-only or alias it to the ordinary text `edit`
   category. Prompt text is not part of a reusable file-scope grant. Existing
   ask/auto/yolo rules apply; the standalone CLI is a direct user operation.
6. Hide native image tools in libtny and SSH contexts, and enforce the same
   restriction during interception. CLI `--ssh` fails cleanly instead of
   accidentally reading or writing local paths. Remote users can invoke a
   separately authenticated CLI on that host. Wasm runs the shared code
   through fetch/filesystem; no browser downloader, CORS workaround or new
   embedding ABI is introduced.

## Verification

The unit suite checks tool discovery, permission prompts/grants, reference
changes, malformed arguments, hidden profiles and interception equivalence.
The integration suite drives both operations through standalone CLI, typed
calls, and terminal/terminal+edit agents using a different conversation
provider. It verifies byte-preserving uploads, separate credentials, metadata
results, prompt discovery, refresh, and cancellation without another model
request. Help-parser checks include the shared image option parser; wasm CI
runs the image fixtures with only native-specific signal/filesystem skips.

See [images.md](../images.md) for the user contract and
[ADR 0072](0072-extensible-image-service.md) for provider and persistence choices.
