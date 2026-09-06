# 0074 — Extensible image service with a Codex adapter

Date: 2026-09-05
Status: accepted

## Context

The Codex CLI's release-pinned Images client supports standalone generation
and editing using ChatGPT authentication. tny needs those operations regardless
of its current chat provider and must be able to add providers such as Grok
without duplicating CLI, tool or persistence behavior.

## Decisions

1. Introduce `tny_image_request` / `tny_image_result` and the private
   `tny_image_provider` adapter contract. A small static registry selects an
   adapter by name, independently of chat configuration. Adapters declare a
   default model, maximum references and local credential availability; a
   single render callback receives loaded input bytes and returns output
   bytes. Zero references are generation; edit is explicitly selected and
   must have references. New providers need an adapter and registry entry,
   not changes to the command or tool dispatch. Schema gating and agent
   provider discovery iterate the same table. Avoid dynamic plugin loading,
   new dependencies, and premature unification with the MP3 speech service.
2. The first adapter is `codex`, pinned to OpenAI Codex
   `rust-v0.154.0-alpha.3`, with default `gpt-image-2`. It uses
   `POST /backend-api/codex/images/generations` or `/images/edits`, JSON
   inputs and `data[0].b64_json` output. Edits use an `images` array of
   `image_url` data URLs. Send `background:auto`, `quality:auto`, `size:auto`
   by default; permit image model, quality and size overrides. Do not claim
   public API stability or support for arbitrary response URLs/batches.
3. Reuse Codex auth precedence/refresh (ADR 0066), never the selected chat
   provider's API key or base URL. Only trusted `TNY_CODEX_BASE_URL` redirects
   image requests. Local capability checks neither refresh nor contact the
   service. Authenticated generation is the final entitlement check.
4. Bound UTF-8 prompt to 16 KiB, references to five regular PNG/JPEG/WebP
   files at 8 MiB each, output to 32 MiB, wire JSON to base64 expansion plus
   64 KiB. Read references incrementally with a bound even after stat, and
   open nonblocking to reject FIFOs without hanging. Check base64 alphabet,
   padding, padding bits, decoded length and image magic. No linked image
   decoder: magic checks do not prove complete pixel-level image integrity.
   Reject GIF, missing/multiple outputs, HTML, truncated JSON/body, and errors.
5. Reuse `http_conn` and `tny_poll`. No new platform seam or event loop.
   One five-minute response deadline accommodates image latency; connect,
   upload and refresh retain existing transport/auth behavior. Honor the
   caller's cancellation callback through I/O and before commit. Never
   automatically retry a paid image request. Report HTTP status, not raw
   provider error bodies, to keep credentials/content out of diagnostics.
6. Shared service owns artifact persistence: require one explicit output,
   reserve a mode-0600 sibling temporary file before sending, write only after
   a complete validated response, and rename on success. Existing regular
   outputs may be replaced; special outputs fail. Load all edit references
   first to support in-place editing. Always remove owned temporaries on
   ordinary error/cancellation. Atomic visibility is promised, not fsync-based
   crash durability; SIGKILL can leave a private temporary file. Do not put
   binary artifacts into the one-deep text undo slot or auto-attach pixels.

## Consequences and verification

Chat and image providers are independent. Grok is an extension point, not an
implemented provider. No extra runtime process or SDK is needed. Base64 JSON
costs memory but stays explicitly bounded. Output MIME is reported from bytes;
file extensions do not perform conversion. The core stays in SRC_SHARED;
wasm works through fetch and its filesystem, subject to browser CORS/auth.

Unit and HTTP integration tests cover validation, bounds, credential
isolation and refresh, request shape, byte-split chunking, malformed and
oversized responses, no-retry errors, atomic replacement and interruption.
The existing CI matrix covers native architectures and wasm.

## Primary sources

- [Images client](https://github.com/openai/codex/blob/rust-v0.154.0-alpha.3/codex-rs/codex-api/src/endpoint/images.rs)
- [Request and response types](https://github.com/openai/codex/blob/rust-v0.154.0-alpha.3/codex-rs/codex-api/src/images.rs)
- [Tool defaults](https://github.com/openai/codex/blob/rust-v0.154.0-alpha.3/codex-rs/ext/image-generation/src/tool.rs)
- [ChatGPT base URL selection](https://github.com/openai/codex/blob/rust-v0.154.0-alpha.3/codex-rs/model-provider-info/src/lib.rs)

Live acceptance on 2026-09-05 used the existing ChatGPT login: generation
returned a 759,985-byte PNG in 12.80 s, then editing returned an 813,445-byte
PNG in 15.37 s. Visual inspection confirmed an orange circle became blue while
preserving the white background and composition. Both commands exited zero.
These are observed smoke-test timings, not latency or performance claims.
Credentials and generated images are not part of the commit.

Final local gates: `make quality`, `env -u TNY_TOOLS make test` (451 unit
tests / 11,348 assertions and 41 integration groups), and
`env -u TNY_TOOLS make leaks` passed. Removing the workstation's
`TNY_TOOLS=terminal` override restores the fixture suite's default environment;
the image fixtures explicitly test all supported tool profiles. The targeted
mutation pass caught all ten compilable mutations (nine unit kills, one
integration kill); seven were rejected by the compiler. The restored image
unit/integration suites passed again. Stripped macOS arm64 size: 867,312 bytes.

The provider lookup is explicitly checked before dereference. GCC 14's
path-sensitive analyzer passes all nine changed runtime translation units.
Quality, the full tests (including the newly merged Zsh quick-ask suites), and
leak checks passed again after integration with main.
