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
| `--strict-size` | Fail instead of saving unless the image is exactly the requested `WIDTHxHEIGHT` |
| `--image PATH` | Repeatable edit reference, in priority order |
| `--artifact RECORD` | Repeatable edit reference taken from an earlier manifest's verified output |
| `--manifest RECORD` | `replay` only: the operation to rerun |
| `--no-manifest` | Write no manifest, prompt or reference record for this run |
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
{"kind":"image","ok":true,"operation":"generate","provider":"codex","model":"gpt-image-2.5-sunburst","path":"robot.png","mime_type":"image/png","bytes":123456,"requested_size":"3440x1440","effective_size":"3440x1440","width":1935,"height":811,"size_status":"mismatch","native":true,"transform":null,"operation_id":"4e531a053753dd2f","manifest_path":"/work/robot.png.tny-image-4e531a053753dd2f.json","seed":null,"request_id":null}
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

## Requested versus actual dimensions

Four values are kept apart, because a provider may substitute a size silently:

| Field | Meaning |
| --- | --- |
| `requested_size` | The literal you asked for; `auto` when `--size` is omitted |
| `effective_size` | The exact literal the adapter put in the request body; `null` if none was sent. It is never a guess about provider internals |
| `width`, `height` | Read from the returned image's own header; `null` when unreadable |
| `size_status` | `match`, `mismatch`, `auto`, `unverifiable` or `unsupported` |
| `native`, `transform` | `true` / `null`: tny saves the provider's own bytes and performs no resize, crop or re-encode |

`size_status` is decided in this order:

1. `auto` — no concrete size was requested (omitted or `auto`); nothing is compared.
2. `unsupported` — the image is a recognized PNG/JPEG/WebP container whose
   dimension encoding this build does not read, such as a lossless JPEG frame or
   a WebP with no `VP8`/`VP8L`/`VP8X` canvas chunk.
3. `unverifiable` — a truncated, malformed, inconsistent or impossible header,
   or an opaque provider size token whose dimensional meaning is unknown. An
   opaque token is never resolved into a match from the returned bytes.
4. `match` / `mismatch` — exact comparison of **both** width and height against
   the requested `WIDTHxHEIGHT`. A matching aspect ratio is not a match.

A mismatch (or a concrete request whose dimensions could not be read) prints one
actionable warning to **stderr**; stdout keeps its existing content, so scripts
reading the path or the JSON object are unaffected. Nothing is retried.

Dimensions come from a bounded header reader next to the existing MIME sniffing:
PNG's IHDR, a JPEG marker/segment walk to a supported frame header, and RIFF/WebP
chunk walking. Every length, segment and chunk bound is checked before a read; a
truncated or impossible header yields no dimensions rather than a guess. This is
**header metadata, not pixel decoding**: it does not prove the image data decodes,
and tny links no image decoder to answer the question. Accepting a file as an
image still depends only on its magic bytes, unless `--strict-size` is used.

### Strict size

`--strict-size` (`strict_size` in the tools and SDKs) requires a concrete
positive `WIDTHxHEIGHT` request **and** returned bytes that verifiably match it:

- `auto`, an omitted size, an opaque token or an invalid size is rejected
  **before any request is made**, so no quota is spent. Code:
  `IMAGE_STRICT_SIZE_INVALID`.
- A mismatch fails with `IMAGE_SIZE_MISMATCH`; unreadable dimensions fail with
  `IMAGE_SIZE_UNVERIFIABLE` or `IMAGE_SIZE_UNSUPPORTED`.
- In every failing case the destination is **not** replaced — any previous file
  keeps its exact bytes — and the paid response is discarded rather than saved,
  resized or retried with another size or model. That cost is the point of the
  flag: you pay for the request and deliberately throw the result away.

Exit status is 1. With `--json`, the failure is a stable object that never
claims a committed output:

```json
{"kind":"image","ok":false,"operation":"generate","code":"IMAGE_SIZE_MISMATCH","error":"IMAGE_SIZE_MISMATCH: requested 3440x1440 but the provider returned 1935x811; no file was written","mime_type":"image/png","requested_size":"3440x1440","effective_size":"3440x1440","width":1935,"height":811,"size_status":"mismatch","path":null,"committed":false}
```

Without `--json` the same message goes to stderr with its code, and stdout stays
empty.

That object is built entirely from the four codes above plus locally known
values: the size you asked for, the literal actually sent (`effective_size` is
`null` when nothing was sent), dimensions and MIME read from the returned bytes,
`path: null` and `committed: false`. It never contains credentials, endpoints,
headers, provider response text, prompts, or reference/output paths.

Exactly one other failure carries structured detail: `IMAGE_MANIFEST_FINALIZE_FAILED`,
where the image **was** written and only its record could not be finalized (see
[order of operations](#order-of-operations-and-what-survives-a-crash)). Its
object is distinct, not a strict-size failure with different values: it reports
`committed: true`, the real `path`, `bytes`, and this operation's
`operation_id`/`manifest_path` alongside the same locally decided size and MIME
metadata. Every other failure — provider, configuration, transport,
cancellation, out of memory — keeps its ordinary message and carries no detail
at all.

Both objects reach every caller through its existing failure channel:

- **Typed tools** (`image_generate` / `image_edit`) keep the usual `error: `
  failure marker; for these five codes the bytes after that prefix are the JSON
  object. It is still a failed tool result, never a success.
- **Intercepted shell** `tny image …` returns it on stdout only when the command
  explicitly asked for `--json`; plain-text runs keep an empty stdout. The exit
  status stays nonzero either way and nothing is re-requested.
- **SDKs** keep their stable error category and generic message, and attach the
  same values as an optional read-only failure object: `image_detail`
  (`ImageFailureDetail` or `RetainedImageDetail`) in Python and non-enumerable
  `imageDetail` in TypeScript — see [docs/sdk-toolkit.md](sdk-toolkit.md). The
  two shapes are discriminated by `committed`, never parsed by one reader.
  Ordinary printing, `repr`, tracebacks, `JSON.stringify` and inspection never
  show either. A retained artifact is reported as an I/O failure rather than
  cancellation, even if the job is cancelled after the file was committed.

### Provider size support

tny does not ship a catalog of provider sizes and does not treat the public
OpenAI Images API as a description of the ChatGPT account endpoint used here.
A size is forwarded literally; the provider decides. If it rejects the size, the
request fails with its HTTP status and is never retried with a different size or
model. If it substitutes a size instead, that shows up as `mismatch` with the
actual dimensions — the observed behavior behind issue #122, where 3440x1440
requests returned roughly 1935x811 images. Verify a size by requesting it once
and reading `width`/`height`, or enforce it with `--strict-size`.

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

## The pending-image queue and explicit previews

One queue carries every image that rides the next native provider request
([ADR 0096](adr/0096-captured-image-queue-and-preview-lifecycle.md)). Each
entry records its canonical path, the **exact bytes loaded when it was
admitted**, their length, MIME and SHA-256, and whether it came from a manual
attachment (`read_image`, `tny image attach`, terminal interception) or from an
explicitly requested generated-image preview. Nothing re-reads the path at
flush time, so two generations that write the same `--output-file` inside one
tool batch attach their own versions, in queue order, rather than the later file
twice. The existing bounds are unchanged: at most eight entries per step and
8 MiB per image. The limit applies to the actual opened-file read, including
files that grow while loading, and to the loaded-image builder. The `--ssh`
remote `read_image` retains the fetched bytes too. Its staged local copy is
provenance only, never the source of a later upload.

The flush sends every queued entry exactly once, as the one established
`image_url` user message after the `role:"tool"` results, with truthful text:

| Batch | Text |
| --- | --- |
| all manual | `Image attached by read_image.` |
| all preview | `Images queued by explicitly requested generation/edit preview.` |
| mixed | `Images attached by explicit tool requests.` |

None of them says a model looked at anything. **Queued is a receipt at that
moment, not perception**: a cancellation, a policy change or a failed turn can
still prevent delivery, and that is reported rather than implied.

Preview admission is deliberately stricter than manual attachment. It requires
`image_input` **configured true** for the effective provider (unknown is not
support), a path inside the allowed roots, and an `expected_sha256` that matches
the bytes just captured — never a second read of the path. It also requires the
owning native OpenAI-compatible backend to be inside a tool batch that can still
make another request: a turn that is merely active and streaming, cancelled,
denied or out of step budget answers `turn_not_ready` instead of `queued`.

A queued preview that can no longer be delivered fails the batch as a whole. The
flush checks every entry before mutating anything; if one preview has become
incompatible it preserves all entries, bytes and the count, and the owning
backend then reports `IMAGE_PREVIEW_NOT_DELIVERED`, makes no further provider
request, ends the turn failed, and only afterwards releases the batch — so no
stale pixels survive into a later turn and nothing holds the 64 MiB bound
indefinitely. A cancelled, stopped, denied or step-limited turn that had
accepted a preview reports the same non-delivery for that turn. Terminal cleanup
also covers tool-result persistence failure, provider-request extension stops,
and hard cancellation. A constructed but unsent image message is removed from
the transcript, not just from the pending queue. Only successfully submitted
bytes remain as history; submission is not proof of perception. Control op,
role, id, path and hash validation uses the full decoded JSON string and rejects
embedded NUL. Reply correlation also requires an exact full-string id match.
Manual-only
batches keep their existing behavior: a refused flush preserves its entries and
the turn continues with a warning.

There is no `tny image generate --preview` yet: this slice implements the queue,
the shared builder, the control receiver and the reusable control primitive
only. The generation surface, derived bounded previews and manifest lineage
arrive with the integration slice.

## Generation manifests and lineage

Every generate, edit and replay writes one private record of that operation
next to its destination, named `<output>.tny-image-<operation-id>.json`
([ADR 0088](adr/0088-image-dimensions-and-generation-manifests.md)). Its path is
returned as `manifest_path` and printed on **stderr**; stdout is unchanged. It
is mode 0600, created exclusively, and once terminal it is never rewritten:
generating again at the same path creates a **new** record rather than
overwriting the earlier lineage. There is no registry, no shared "latest"
pointer and no copy of the image bytes anywhere else.

```json
{"version":1,"kind":"image_manifest","operation_id":"4e531a053753dd2f","operation":"edit",
 "status":"succeeded","workspace":"/work","started":"2026-09-12T08:00:00Z",
 "finished":"2026-09-12T08:00:21Z","prompt":"Make the robot blue","output":"/work/blue.png",
 "committed":true,
 "references":[{"path":"/work/robot.png","sha256":"…","source_manifest":null,"source_operation":null}],
 "requested":{"provider":"codex","model":null,"quality":"high","size":"1024x1024"},
 "effective":{"provider":"codex","model":"gpt-image-2.5-sunburst","size":"1024x1024"},
 "result":{"width":1024,"height":1024,"mime_type":"image/png","bytes":123456,"size_status":"match"},
 "actual":{"seed":null,"request_id":null},"error":null,"source":null,
 "artifacts":[{"role":"native","path":"/work/blue.png","sha256":"…","width":1024,"height":1024,
               "mime_type":"image/png","bytes":123456,"transform":null,"source_operation":null}]}
```

`status` is `running`, `succeeded`, `failed` or `cancelled`. A reader adds a
fifth, **observed** state: a `running` record whose destination has no live
writer holding it for that same operation is `interrupted`. An abandoned
operation is never reported as success just because a file exists at its path.

`references[].sha256` is the hash of the **exact bytes uploaded**, taken from
the single bounded read that produced them — never from an earlier preflight
that a later write could race. Reference hashes, the artifact hash and the
result dimensions are the only things a later run trusts: if the file at a
recorded path no longer hashes the same, tny refuses rather than uploading
whatever now sits there.

`actual` holds only identifiers the provider genuinely returned — currently a
scalar `seed` and a `request_id`. Absent means `null`. No local operation id,
requested seed or timestamp is ever promoted into one, and no unrecognized
provider field is copied into the record.

### Order of operations and what survives a crash

Before any paid request, tny normalizes the destination, refuses unsafe
aliases, takes a **nonblocking writer guard** for that destination, reads and
hashes the references, and writes the private `running` record. A second
operation on the same normalized path fails with a stable busy error *before*
a request is made, so a race costs nothing. If the record cannot be created,
the run fails before contacting the provider — no quota is spent.

Then, under that guard, the returned bytes are validated, `--strict-size` is
applied, the image is atomically replaced, and the record is atomically
finalized. These are two independent renames, deliberately **not** a
transaction, and tny does not pretend otherwise:

- If finalizing the record fails after the image is committed, the run exits
  nonzero with the distinct code `IMAGE_MANIFEST_FINALIZE_FAILED` and says the
  artifact was **written and kept**. It is never deleted to make a metadata
  failure tidy, and this is not a strict-size failure: `committed` is `true`
  and `path` names the real file. Every caller — CLI, typed tools, intercepted
  shell and the SDK toolkit — reports that retained artifact rather than a bare
  failure, so nothing has to guess whether the file exists.
- If the process dies in between, the `running` record stays. A reader sees
  `interrupted`, and replay refuses it.
- Provider, validation, strict-size and cancellation failures record a safe
  terminal `failed`/`cancelled` state with a whitelisted code and message, no
  artifact, and no change to whatever the destination held before.

The guard is a lock file under `~/.tny/image-guards/`, keyed by a hash of the
canonical destination. Ownership is the live lock handle, not a stored process
id, so normal exit, cancellation and a killed process all release it. It holds
only an operation id and a pid — never a prompt, reference or credential — and
is removed on release under the lock, which a contender revalidates by inode so
removal can never leave two writers of one destination.

### Destinations and aliases

A destination must resolve to a plain file name inside an existing directory.
These are refused before anything is written or requested:

- a symlink, directory, device or FIFO;
- a name with **more than one hard link**, because tny replaces the name
  atomically and will not record an ambiguous artifact identity;
- a reserved `*.tny-image-<id>.json` record name;
- for a replay or `--artifact` edit, the recorded artifact itself, or the
  record being rerun — replay requires a genuinely new output.

Editing in place with an explicit `--image` path stays supported and is the one
documented case where a reference and the destination are the same file: every
reference is fully loaded and hashed before anything is written, so the upload
is the old file and the record keeps its hash.

### Privacy and the opt-out

`--no-manifest` (`persist_manifest: false` in the tools and SDKs) writes **no**
manifest, no prompt and no reference record, and creates nothing to replay. The
writer guard may still be taken; it never contains prompt or reference data.
Credentials are never written to a record in either mode: the record holds
provider and model names, sizes, paths, hashes and locally chosen error codes.
Records are readable only by their owner, but they are ordinary files in your
workspace — a prompt you would not commit is a prompt to generate with
`--no-manifest`.

### Replay and earlier artifacts

```sh
tny image replay --manifest robot.png.tny-image-4e531a053753dd2f.json \
  --output-file robot-again.png --quality max
printf 'Make it blue' |
  tny image edit --artifact robot.png.tny-image-4e531a053753dd2f.json \
    --output-file blue.png
```

`replay` reruns the recorded operation with its stored prompt, references and
settings. A new `--output-file` is mandatory. Any of `--image-provider`,
`--model`, `--quality`, `--size`, `--strict-size` and `--no-manifest` given on
the command line replaces its recorded counterpart, and **nonempty** piped
stdin replaces the recorded prompt; empty or absent stdin keeps it, so a pure
replay needs no input on a terminal. The new record links its source operation.

`--artifact RECORD` uses that record's verified successful output as a
reference. `--image PATH` keeps working, `--artifact` and `--image` share one
ordered list and the same maximum of five, and the record stores each
reference's original path, hash and — where it came from one — the source
record and operation id.

Replay and `--artifact` fail **before any request** when the record is missing,
blank, malformed, oversized, a newer schema version, or records a failed,
cancelled or interrupted operation, and when a required reference is missing or
no longer matches its recorded hash. Supply that reference explicitly if the
replacement is intended. Relative paths in a record resolve against the
record's own `workspace`, never the directory you happen to run from.

Reading a record never opens a referenced file and never contacts a provider;
only an explicitly requested replay or edit does, under the same permission
boundary as any other image operation. A reference that a record introduces is
additionally confined to the directories the runtime already allows, so a
hostile record cannot widen tny's reach, and the permission grant enumerates
the real resolved paths that would be uploaded.

A record edited between the approval and the run cannot redirect that run
([ADR 0095](adr/0095-owned-image-plans-and-retained-failure-detail.md)). The
tool and interception paths resolve the operation **once**, before the
permission question, and keep that resolved plan — prompt, provider, model,
quality, size, reference paths and the hashes each reference must still have —
until the call finishes. The approved plan is then what runs: the record is not
reopened, no second permission question is asked (so a one-time approval stays
one-time and grants nothing), and rewriting the record afterwards changes
neither the settings nor the paths used. What it cannot do is make stale bytes
acceptable: every reference is hashed as it is loaded, and a reference whose
content changed fails before the request. A permission-gated call that arrives
without its prepared plan is refused rather than resolved again.

Because the description is taken from the resolved plan, a replay is described
with the provider it actually inherited from its record rather than the default
`codex`. This is a deliberate fail-closed correction: an old rule or grant
written against the previously described provider no longer authorizes such a
replay, and must be granted again. Nothing else about the detail shape changed.

Export and job lineage (`role: "derived"`, `--job`) are **not** implemented
yet; they arrive with the export (#125) and job (#124) slices. `artifacts`
therefore currently holds exactly one `native` entry per successful operation.

## Conversation image input (`image_input`)

Sending images *to the conversation provider* is configured separately from
generation. One additive top-level `~/.tny/settings.json` object maps
canonical provider selectors to booleans
([ADR 0089](adr/0089-image-input-policy-and-shared-gates.md)):

```json
{
  "image_input": { "codex": true, "claude": false, "my-gateway": true, "acp@agent": false }
}
```

It is a separate map, not a field inside a provider object, so configuring
image input can never shadow a builtin subscription profile's OAuth/token
wiring. Keys are provider selectors of 1–256 bytes using the existing
provider-name grammar (letters, digits, `-`, `_`); named ACP agents use
`acp@NAME`, and the legacy `acp:NAME` selector resolves to the same key.
`image_input` is a reserved key: `tny provider setup image_input` is refused.

Three states, resolved for the effective provider and recomputed on every
provider switch:

| Value | State | Behavior |
| --- | --- | --- |
| absent | unknown | Existing explicitly requested image paths keep working. tny never reports this as verified support and it cannot authorize an automatic preview. |
| `true` | **configured, unverified** | Your assertion about this profile. tny performs no live entitlement, model or transport check. |
| `false` | configured off | Hard refusal of conversation image input for that provider. |

A `false` provider refuses at every shared boundary, with the one message
`image input is disabled for this provider by settings.json image_input`:

- `tny ask --image PATH` fails with exit 1 before a session is created or
  opened and before the provider is contacted;
- every turn entry point (TUI, one-shot CLI, detached runner, library callers
  and native subagents) refuses an image turn before prompt, event or session
  state changes;
- `read_image` disappears from the advertised tool schema and the same
  condition refuses a direct call, including `--ssh` tool runs;
- the image queue (`read_image`, `tny image attach`, terminal interception,
  explicit preview) refuses before the path is resolved or read, and a pending
  queue that became refused is *not* flushed — its entries and count are
  preserved.

Only `true` authorizes an automatic generated-image preview; `absent`/unknown
keeps the manual paths working and answers `unsupported` to a preview request
(see [the pending-image queue](#the-pending-image-queue-and-explicit-previews)).

A `true` never overrides an actual restriction: the ACP client still rejects
image prompts, image attachment stays native-loop only, and tool profile,
library/`--ssh` and permission rules apply unchanged. The map is validated
strictly when settings load: a non-object root, a non-boolean value, a
repeated key, an embedded NUL, a selector outside the grammar or more than
1024 entries fails configuration with a stable diagnostic instead of an
ambiguous lookup.

Image **generation** and editing are never gated by this map: they use their
own image provider and credentials and stay available when the conversation
provider cannot take pixels.

## Agents and platforms

The `all` tool profile exposes `image_generate` and `image_edit` when a registered image adapter has local credentials (initially ChatGPT). They take `prompt`, `output_file`, optional
`provider`, `model`, `quality`, `size`, boolean `strict_size`, boolean
`persist_manifest`, `from_manifest`, and (edit only) an `images` string array
and an `artifact` record path. `prompt` is required unless `from_manifest`
reuses a recorded one, and a `from_manifest` record must name the same
operation as the tool, so a generate can never quietly become an upload.
They share the CLI's Sunburst / `high` defaults and model/quality overrides.
Shell profiles retain their existing schema and receive instructions for the
CLI commands. Simple piped/quoted-heredoc `tny image generate/edit` commands
are intercepted and run in process with the same provider, cancellation, and
permission engine as typed tools; unsupported shell grammar stays with the
terminal executor under its normal shell permissions.

Both operations are sensitive tools, with distinct `image_generate` and
`image_edit` permission identities. Grants include the image provider actually
resolved for the call, the canonical output path, any record being rerun, and
every uploaded reference path — resolved from records before the grant, not the
raw argument, and then kept and run exactly as approved. Changing
references requires a new grant in ask mode. An intercepted
`tny image replay` reads its record to choose the identity of the operation it
would actually run, so replaying an edit needs an `image_edit` grant. `strict_size` only narrows what may be saved,
so it is deliberately not part of the grant identity and never widens a grant. These identities are not read-only or the
ordinary `edit` permission category: authorizing text writes alone does not
authorize image requests. Yolo mode continues to allow them.

Native CLI, TUI and ACP-server native turns can use the tools with any chat
provider. Host-owned Cursor/ACP client agents can discover the standalone CLI
through `tny image --help`; tny does not inject native tools into their loops.
Image tools are unavailable in libtny and `--ssh` tool runtimes, with a clean
error instead of accidental local file access. Run the CLI on the remote
machine with its own credentials if remote image work is needed.

Wasm uses the existing fetch transport and virtual filesystem with the same
CLI/service implementation, including the dimension reader, `--strict-size`,
manifests and replay: they are plain shared C with no host dependency. The one
deliberate difference is the writer guard. Emscripten has no advisory file
locking and its filesystem is per-instance, so a `flock` stub would answer
"acquired" for every contender; tny therefore keeps the guard in a table inside
the running instance, which is exactly the scope in which two concurrent tny
image operations can exist there. It is a real guard for that scope and is not
described as an OS lock. Node wasm fixtures cover generation/editing, dimension
metadata, strict size, records and replay through agent calls; native signals,
shell interception, cross-process concurrency and FIFO/symlink tests are not
applicable there.
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
[ADR 0084](adr/0084-codex-image-defaults.md),
[ADR 0088](adr/0088-image-dimensions-and-generation-manifests.md),
[ADR 0096](adr/0096-captured-image-queue-and-preview-lifecycle.md). Fixture
verification: `tests/test_image_service.c`, `tests/integration/test_image_service.py`
and `tests/integration/test_image_workflow.py` (`-k Dimensions` for dimensions
and strict size, `-k Manifest` for records, replay and lineage). The captured
queue, its preview admission and the control receiver are covered by
`tests/test_core.c`, `tests/test_openai.c` (a loopback provider asserting the
actual next request's pixel bytes), `tests/test_runner.c` and
`tests/integration/test_image_preview_queue.py`.
