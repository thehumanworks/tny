# Image generation and editing

Generate images with your ChatGPT login independently of the conversation
provider. Sign in with `tny --provider codex login`, then:

```sh
printf 'An orange robot reading under a tree' |
  tny image generate --output-file robot.png --json
printf 'Make the robot blue; preserve the composition' |
  tny image edit --image robot.png --output-file blue.png --json
```

The prompt is plain UTF-8 stdin, not an argv argument or JSON input. It must
be nonblank, contain no NUL, and fit in 16 KiB. `--output-file` is required.
`edit` takes one to five repeated `--image` paths (regular PNG/JPEG/WebP files,
8 MiB each); `generate` rejects reference images. Reference files are uploaded
to the image provider. Their MIME type comes from bytes, not file extensions.

`tny image generate --check --json` checks local credential presence only;
it does not read stdin, refresh credentials, or spend quota. It cannot verify
server entitlement or token validity. `tny image attach PATH` keeps its existing
session-socket behavior; generate/edit are standalone and require no socket.

## Options and output

| Option | Contract |
| --- | --- |
| `--image-provider NAME` | Image adapter, default `codex`; unsupported names fail before network I/O |
| `--model NAME` | Image model; Codex defaults to `gpt-image-2.5-sunburst`, independently of global chat `--model`; override with e.g. `gpt-image-2.5-flare` |
| `--quality LEVEL` | `auto`, `low`, `medium`, `high` (default), `xhigh`, `max` |
| `--size SIZE` | Provider-specific size string, e.g. `1024x1024`; default `auto`; forwarded as a provider hint, without local resizing |
| `--image PATH` | Repeatable edit reference, in priority order |
| `--output-file PATH` | One explicit destination, replaced atomically after success |
| `--check` | Local provider capability/credential check |
| `--json` | Structured result metadata, never base64 or raw image bytes |

Generation and editing both default to Sunburst with `high` quality. Set model
and quality separately; selecting Flare still defaults to `high` unless you
also pass `--quality`. Explicit `auto` is forwarded unchanged. For example:

```sh
printf 'An orange robot reading under a tree' |
  tny image generate --model gpt-image-2.5-flare --quality medium --output-file robot.png
printf 'Make the robot blue; preserve the composition' |
  tny image edit --image robot.png --quality xhigh --output-file blue.png
```

Both models support these six quality levels in the
[OpenAI image prompting guide](https://developers.openai.com/api/docs/guides/image-prompting).
Model names remain provider-defined strings; other explicit image models are
forwarded for the provider to validate. A provider rejection never silently
falls back to another model or quality.

Global `--cwd DIR` sets the filesystem base. Chat `--provider`, `--base-url`
and API keys do not select or redirect the image adapter. Global
`--chatgpt-token` / `--chatgpt-account-id` retain their credential precedence.

Plain output is the destination path followed by a newline. JSON success:

```json
{"kind":"image","ok":true,"operation":"generate","provider":"codex","model":"gpt-image-2.5-sunburst","path":"robot.png","mime_type":"image/png","bytes":123456}
```

Check output is `{"kind":"image","available":true}` (or false). Errors go to
stderr. Exit codes: **0** success, **1** invalid configuration/input, file or
response failure, **2** HTTP rejection, **130** interruption. A file produced
before a stdout error may still exist; check the exit status and filesystem.

The image bytes determine `mime_type` (PNG/JPEG/WebP); filenames do not convert
formats. Size is a provider hint: the 2026-09-05 `gpt-image-2` smoke test
returned a 1254x1254 PNG for a 1024x1024 request. Exactly one image is accepted;
batch generation and URL-based outputs are deliberately absent. An adapter
must return validated bytes rather than ask the caller to fetch an arbitrary URL.

## Persistence and cancellation

A private sibling temporary file is reserved before the provider request.
An existing destination remains unchanged on failed, malformed, oversized,
truncated, or cancelled responses. Successful output is mode 0600 on POSIX.
CLI destinations must be regular files or new paths, not directories or leaf
symlinks. Typed tools first use tny's usual workspace path resolution.
Parent directories must already exist. Editing in place is supported: all
references are loaded before output is replaced.

The service bounds decoded output at 32 MiB and JSON wire bytes at the base64
expansion plus 64 KiB. One five-minute deadline covers response headers and
body. HTTP connection establishment/request writes and credential refresh use
the existing transport/auth deadlines. Cancellation is checked while reading
stdin, loading references, waiting for the response, writing, and immediately
before rename. Completed requests are never automatically retried.

Generated files are explicit artifacts, outside the session's text-edit undo
slot; `/undo` does not remove or restore them. Choose a new destination to
retain previous images. No image bytes are stored in tool result transcripts;
use `read_image` or `tny image attach` explicitly to add them to model context.

## Agents and platforms

The `all` tool profile exposes `image_generate` and `image_edit` when a registered image adapter has local credentials (initially ChatGPT). They take `prompt`, `output_file`, optional
`provider`, `model`, `quality`, `size`, and (edit only) an `images` string array.
They share the CLI's Sunburst / `high` defaults and model/quality overrides.
Shell profiles retain their existing schema and receive instructions for the
CLI commands. Simple piped/quoted-heredoc `tny image generate/edit` commands
are intercepted and run in process with the same provider, cancellation, and
permission engine as typed tools; unsupported shell grammar stays with the
terminal executor under its normal shell permissions.

Both operations are sensitive tools, with distinct `image_generate` and
`image_edit` permission identities. Grants include the image provider,
canonical output path, and every uploaded reference path. Changing references
requires a new grant in ask mode. These identities are not read-only or the
ordinary `edit` permission category: authorizing text writes alone does not
authorize image requests. Yolo mode continues to allow them.

Native CLI, TUI and ACP-server native turns can use the tools with any chat
provider. Host-owned Cursor/ACP client agents can discover the standalone CLI
through `tny image --help`; tny does not inject native tools into their loops.
Image tools are unavailable in libtny and `--ssh` tool runtimes, with a clean
error instead of accidental local file access. Run the CLI on the remote
machine with its own credentials if remote image work is needed.

Wasm uses the existing fetch transport and virtual filesystem with the same
CLI/service implementation. Node wasm fixtures cover generation/editing and
agent calls; native signals and FIFO/symlink tests are not applicable there.
Browser calls additionally depend on provider CORS and an available ChatGPT
credential; there is no CORS bypass or browser download UI in this feature.

## Provider extension point

`core/image_provider.h` defines a small adapter table: name, default image
model, maximum edit references (zero means generation-only), local capability
check, and a render callback receiving already loaded image bytes. Schema gating and agent provider discovery also iterate this registry. Add an
adapter and one registration in `image_service.c` to support another provider
such as Grok. Keep authentication, URL construction, provider payloads and
response decoding inside the adapter. CLI parsing, permissions, limits and
atomic persistence remain shared. No Grok image support is claimed yet.

The Codex adapter uses ADR 0066's ChatGPT credentials and refresh, including
flag > environment > tny store > Codex store precedence. Only
`TNY_CODEX_BASE_URL` can override its base URL (default
`https://chatgpt.com/backend-api/codex`); the override is trusted configuration.
It posts JSON to `/images/generations` or `/images/edits`, authenticates with
OAuth bearer plus `chatgpt-account-id`, and decodes `data[0].b64_json`.
API-key-only credentials are insufficient for this adapter. Request/error
bodies and credentials are not printed. The client contract is pinned to
[Codex rust-v0.154.0-alpha.3](https://github.com/openai/codex/blob/rust-v0.154.0-alpha.3/codex-rs/codex-api/src/endpoint/images.rs).
These are Codex backend routes, not a stable public third-party API contract.

Built-in image generation consumes Codex subscription allowance; it is not
unlimited image generation. See [OpenAI's image documentation](https://learn.chatgpt.com/docs/image-generation).

Architecture: [ADR 0074](adr/0074-extensible-image-service.md),
[ADR 0075](adr/0075-image-cli-and-agent-tools.md),
[ADR 0084](adr/0084-codex-image-defaults.md). Fixture verification:
`tests/test_image_service.c`, `tests/integration/test_image_service.py`.
