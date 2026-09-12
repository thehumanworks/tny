# Image generation and editing

## Explicit conversation preview (ADR 0097)

Generation, editing, replay, export and contact-sheet remain **metadata-only by
default**. Add `--preview` to the CLI command, or boolean `preview: true` to its
native tool. `preview: false` preserves the ordinary permission identity. A
preview permission adds the conversation profile and model to that identity;
the already-approved image plan runs once. ALLOW_ONCE is not remembered.

To preview an existing artifact without producing anything:

```sh
tny image preview --manifest result.png.tny-image-ID.json --json
```

The equivalent native tool is `image_preview({"manifest":"RECORD"})`.
Terminal interception uses the same prepared selection. The selector owns the
successful record before permission. Capture must match its artifact digest,
not the hash of a later replacement at the same path. Derived selections retain
their transform/source lineage and are never relabelled native. To select an image
job, use `tny image preview --job ID --item N --json` or
`image_preview({"job":"ID","item":N})`. Exactly one manifest or complete job/item
pair is required. Only the selected item must have succeeded; siblings may still
run or fail. Selection reads bounded confined metadata without projecting job
state, opening image bytes or initializing a provider (ADR 0098).

One shared coordinator uses the actual owning native backend's admission, or
one non-printing correlated CLI control exchange. Preview requires configured
`image_input: true`, a continuable active tool batch, allowed roots, supported
image bytes, at most 8 MiB per image and eight queued images. Unknown is not
support. Producer SHA-256 comes from the validated bytes before commit, even
with `--no-manifest`. Accepted bytes are immutable until flush or cleanup.

Explicit requests add a separate `preview` object: `status` is `queued`,
`unsupported`, `unavailable_session`, `turn_not_ready`, `failed`, or
`not_attempted`. It reports the selected path, digest, operation/record identity,
`original_bytes` representation and an actionable fallback. A CLI queued
receipt is correlated to its control request. A tool result is correlated by
its ordinary tool-call id. Neither receipt proves delivery, inspection or
visual approval. Missing or uncorrelated acknowledgment is never `queued`.

Successful generation/export keeps its artifact and exit 0 if preview fails;
JSON reports both outcomes, and CLI stderr reports the separate preview
failure. Preview-only failures exit 1. Generation failure queues nothing. A
committed manifest-finalization failure keeps its retained-artifact failure and
reports preview `not_attempted`. On oversize, explicitly export a smaller,
separate artifact and select that record. There is no implicit conversion,
regeneration, retry, or manual attachment fallback.

Cancellation, denial, extension stops, persistence errors, policy changes and
step exhaustion report `IMAGE_PREVIEW_NOT_DELIVERED` before clearing pending
bytes and constructed-but-unsent image messages. A later turn cannot inherit
them. Existing manual attachment semantics are unchanged.

Wasm's native tools use the same backend admission and queue. Socket-only CLI
preview has a clean `unsupported` fallback. External transforms and job controls
remain unavailable there. Native standalone SDK toolkit calls remain
metadata-only and reject `preview` (including false); no public send ABI or
ambient socket lookup is added. Browser-WASM and final job/platform gates are
still required after this independent implementation slice.

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

## Private job publication and producer identity

The private jobs CLI prefix `image --job-no-replace generate ...` selects
atomic creation inside the normal image-service transaction. It is not a
public image option, tool schema field, or SDK ABI addition. Ordinary calls
still replace an existing output only after complete validation.

While holding the canonical output guard, the private mode requires an absent
destination (dangling symlinks also count as existing) and probes hard-link
publication capability in that directory **before contacting the provider**.
Publication links the validated temporary file to the real final name, without
replacement. A destination created during the response wins; tny fails without
changing those bytes. There is no CLI staging destination or manifest rewrite.
This is not protection against arbitrary parent-directory namespace replacement.

Every generation/edit hashes the exact validated output buffer before commit,
including `--no-manifest`. Digest allocation failure publishes nothing. The
private result retains that producer SHA-256; success and retained-artifact JSON
add `sha256` and `cleanup_warning` fields. Existing fields and stdout path-only
behavior are unchanged. JSON readers must tolerate these additive fields.
Manifests and returned identity describe the real final artifact and operation.

A successful link is committed even if removing its private temporary name
fails. The output is kept, provenance is finalized normally, and the result
reports `cleanup_warning:true` (with CLI guidance on stderr). This is success
with cleanup debt, not `IMAGE_MANIFEST_FINALIZE_FAILED`. If manifest finalization
also fails, the existing retained-artifact error includes the producer hash and
cleanup warning. Neither case deletes the final output; late cancellation does
not undo a committed result or replace its retained IO failure.

Private no-replace publication refuses before spend on wasm and on filesystems
without atomic hard-link support. Ordinary wasm generation/editing and manifests
remain supported; no provider or public grammar behavior changes there.

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

Edits accept `--job ID --item N` as one final reference, after the explicit
`--image`/`--artifact` list, whose existing order is preserved. The total limit
remains five. The typed `image_edit` has matching `job` and integer `item`
selectors; its order is artifact, images, job. Generate/replay reject job
selectors and incomplete pairs fail before upload.

The owned selection pins the producer SHA-256, byte count, canonical path,
optional manifest/operation, job ID, item index, current projection attempt,
producing item attempt and carried origin. A carried item from attempt 1 under
projection 3 reports both truthfully. A declared manifest must exist and match;
explicit no-manifest jobs remain usable without fabricating a record. The same
prepared selection survives ALLOW_ONCE, job/manifest deletion or replacement;
changed image bytes fail before upload. Job previews check both hash and positive
producer byte count against the same captured bytes; the existing control message
carries optional `expected_bytes` for this purpose. Omitted length preserves
ordinary preview behavior; malformed supplied lengths are rejected. Later actions ask again.

Reference and derived-source records support optional `job` provenance:
`{id,item_index,projection_attempt,item_attempt,carried_from_attempt,bytes}`.
IDs are 32 lowercase hex; indices 0-63; positive attempts are bounded signed
integers; an ordinary producing attempt equals the projection with carried
origin 0, or producing equals carried origin below projection. Bytes are a
positive integer at most 32 MiB, paired with the reference hash. Present malformed
provenance fails closed; absence keeps older records valid. Replay copies this
identity and uses stored paths/hashes without reopening a mutable job.

Job selectors are native-only and return a clean unsupported result on wasm;
replay of already recorded provenance remains shared. SDK toolkit direct job
and preview selectors are rejected; ordinary artifact/manifest operations remain
available. Local exports record a `derived` artifact and retain their sources.

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

## Local exports and contact sheets

Resizing, cropping, re-encoding and grids are a separate explicit operation.
Generation and editing never invoke a converter, never re-encode provider bytes
and never depend on anything below being installed.

```sh
tny image export --image photo.jpg --output-file thumb.png --size 256x256
tny image contact-sheet --image a.png --image b.png --image c.png \
  --output-file sheet.png --size 512x512 --columns 2 --labels numbers
```

| Option | Contract |
| --- | --- |
| `--image PATH` | Source image; repeatable for a contact sheet, which keeps the given order |
| `--artifact RECORD` | Source taken from an earlier manifest's verified output, in the same ordered list |
| `--output-file PATH` | Required destination; an existing file fails without `--overwrite` |
| `--size WIDTHxHEIGHT` | Required exact canvas; each edge 1–16384 and at most 64M pixels |
| `--fit fit\|crop\|pad` | Default `fit` |
| `--gravity DIRECTION` | `center` (default), `north`, `south`, `east`, `west`, `northeast`, `northwest`, `southeast`, `southwest` |
| `--background COLOR` | `transparent` (default), `#RRGGBB` or `#RRGGBBAA`; JPEG has no alpha, so its default is `#000000` |
| `--format png\|jpeg\|webp` | Default `png`, forced on the encoder and never guessed from the destination's name |
| `--columns N` | Contact sheet only: 1..sources, default `ceil(sqrt(sources))` |
| `--labels none\|numbers` | Contact sheet only, default `none` |
| `--overwrite` | Replace an existing regular single-link destination file |
| `--no-manifest` | Write no record for this run |
| `--json` | Structured result metadata, never image bytes |

`fit` scales proportionally to contain the whole source, then pads to the exact
canvas. `crop` scales proportionally to cover the canvas, then crops at the
gravity. `pad` never enlarges: it shrinks only as much as needed, then pads.
The output is always exactly `--size`; tny does not round it to an aspect ratio.
PNG output is written as 8-bit RGBA so a padded or transparent canvas is
unambiguous. There is no implicit EXIF orientation correction, no provider
request and no retry.

A contact sheet takes 1–64 ordered sources. Rows are `ceil(sources/columns)`,
each cell is `floor(width/columns)` by `floor(height/rows)`, and the pixels a
floored grid leaves over stay background. Each source is placed in its own cell
with the same policy, gravity and background. `--labels numbers` draws the
1..N index of each source from a fixed bitmap glyph table that tny generates
itself as a private PBM: no font is installed, looked up or required, and no
caller text is ever rendered. Labels are scaled by `min(cell_width,
cell_height) / 64`, clamped to 1..4, giving a box of `scale * (6 * digits + 1)`
by `scale * 9` pixels at the cell's top-left corner. Cells too small for that
box are a validation error rather than a silently unlabelled sheet. The same
inputs, settings and tool version produce identical pixels; that is not a
promise across arbitrary future ImageMagick versions.

### The optional converter

Exports need [ImageMagick 7](https://imagemagick.org)'s `magick` executable on
`PATH`. It is optional, is never installed by tny, and is only ever started by
an explicit export or contact sheet. ImageMagick 6 is not a fallback: tny
resolves the first `magick` on `PATH` (skipping empty and relative entries),
requires a regular executable, pins its canonical absolute path and its
device/inode/size/mtime, and uses that same executable for the version probe,
the conversion and the verification — rechecking the identity before each
start. A missing, too old, or unrecognised executable is an actionable error
that converts nothing and writes nothing.

The converter never receives a user-controlled path. Approved source bytes are
read once, hashed once and copied into a private 0700 staging directory under
generated ASCII names; every input and output is named with a forced coder
(`png:`, `jpeg:`, `webp:`, `pbm:`, `png32:`), and every other argument is a
fixed token or a validated number or enumeration. No shell is involved, so
`@file`, bracket selectors, percent patterns and coder-looking file names are
data, not syntax. The child gets a sanitized environment — no credentials, no
`MAGICK_*` configuration, `HOME` and `TMPDIR` inside the private stage — its
own process group, no stdin and a discarded stderr, and untrusted image
metadata never becomes part of a command.

Limits are the tool's own documented ceilings (`-limit memory 128MiB`, `map
256MiB`, `disk 512MiB`, `thread 1`, `time 60`, `area 64MP`) plus tny's own
75-second wall deadline, the 8 MiB input bound, the 64M pixel canvas bound and
a bounded read of the produced bytes. These are cache and resource limits, not
an OS memory sandbox, and they assume a trusted ImageMagick install: a
maliciously replaced executable is not contained by them. Cancelling stops the
child's whole process group and reaps it before returning. The version probe
also observes cancellation. Every stdout drain iteration checks cancellation
and the wall deadline, even after the direct child exits; reads are bounded and
nonblocking, so continuous output or an inherited writer cannot extend the limit.

### What is committed, and when

Exit status is never the evidence. After the producing invocation succeeds, the
same executable decodes the produced file completely a second time with
`-regard-warnings` into `null:`, and the bytes themselves must report exactly
the requested MIME type and canvas. The validated output is hashed before the
irreversible install. A hash allocation failure therefore preserves the old
destination, including with `--overwrite` and `--no-manifest`. Any failure that
remains possible after installation reports the retained artifact truthfully.

The destination is held by the same canonical writer guard as generation, its
parent directory is opened once and kept, and the leaf is named relative to
that descriptor from then on. The destination may not alias any source by
canonical path, symlink or device/inode — even with `--overwrite` — and a
destination that is a symlink, a directory, a device or a multiply linked name
is refused. The identity observed when the destination was opened is rechecked
with `fstatat(AT_SYMLINK_NOFOLLOW)` immediately before the install, and an
unexpected change is refused. The install itself creates the staged file with
`openat(parent, generated-name, O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,
0600)`, fsyncs it, and then links it into place (`linkat`, so a competing
creator loses atomically with `EEXIST`) or renames over the destination for an
explicit `--overwrite`. Neither step opens, truncates or follows the
destination for writing, so a link substituted after the check is never written
through and the bytes of a source inode cannot be modified by an export.

That guard serializes cooperating tny operations; it is not exclusion against
an arbitrary external writer, and tny does not claim an atomic inode
compare-and-rename against one. What it does guarantee is that originals are
preserved: sources keep their bytes and inode, and a failed, refused or
cancelled export leaves the previous destination byte-identical and no
staging debris behind.

### Derived records

An export writes the same version-1 manifest beside its output, with
`operation` `export` or `contact_sheet`, no prompt, `requested.provider`
`local` — never a network provider — an empty `references` list, and one
artifact whose `role` is `derived` and whose `native` flag is `false`. The
record's `transform` object carries the policy, gravity, background, format,
exact canvas, grid and cell geometry, label mode, the tool and its version, and
the ordered sources with the SHA-256 and `width`/`height` of the exact bytes each
one contributed, plus the manifest path and operation id of any `--artifact`
source. Returned results also include an ordered `transform.source_dimensions`
array. These input dimensions are distinct from the target canvas and native
generation dimensions. The manifest reader preserves them; older version-1
records without them retain unknown dimensions rather than inventing values. Provider
fields that do not exist locally — seed and request id — are null, not invented.

The source record of an `--artifact` export is never edited. A derived record
documents local work, so rerunning it as a provider request is refused with an
actionable message at the CLI, the typed tools and terminal interception; its
artifact is still real, so `tny image edit --artifact RECORD` verifies its hash
and uploads exactly those bytes with the derived record as the reference's
lineage. As with generation, a metadata failure after the artifact is in place
is reported as `IMAGE_MANIFEST_FINALIZE_FAILED` with the committed path and
hash, at every surface, instead of deleting real work to keep the error tidy.

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

The same profile also exposes `image_export` and `image_contact_sheet`, which
depend on the host rather than on an image provider: they are advertised
without any image credentials and hidden only where tny cannot run a local
process — libtny, `--ssh` and wasm — with the same direct error on a direct
call. They take an ordered `sources` array of `{"image": PATH}` and
`{"artifact": RECORD}` entries, `output_file`, `size`, and the optional `fit`,
`gravity`, `background`, `format`, `overwrite`, `persist_manifest` and (sheet
only) integer `columns` and `labels`. Whether the optional `magick` executable
is installed is answered when the tool is called, with actionable guidance —
never by probing while building the schema. `tny image export` and `tny image
contact-sheet` typed into the terminal tool are intercepted into exactly these
tools, with the same identity and service.

Both generation operations are sensitive tools, with distinct `image_generate` and
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

`image_export` and `image_contact_sheet` are sensitive under their own two
identities. An export's grant covers the whole operation: the operation name,
the ordered canonical sources with the SHA-256 of their exact current bytes
(plus the manifest path and source operation id of every `--artifact` input),
the canonical destination, the canvas, policy, gravity, background, format,
`overwrite`, `persist_manifest`, columns and label mode. That identity is
rebuilt and rechecked when the call executes, so a source whose bytes changed,
a record edited after approval, or a different destination or setting needs a
new grant rather than inheriting the old one. Execution resolves one owned plan,
checks the artifact paths against allowed roots, captures the source bytes, and
revalidates that plan's identity. The converter stages those same buffers and
resolved lineage; it never reopens records or sources after this check. A
one-call approval (`ALLOW_ONCE`) covers only that prepared identity and does not
create a session grant. Typed tools and terminal interception share this scope.
Reading a record or a status never authorizes a write.

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

Local exports are the one image capability wasm does not have: there is no
process seam to run an external converter through, so `image_export` and
`image_contact_sheet` are hidden from the schema and the CLI and shared service
reject the operation before opening an input, staging a file or touching a
destination. Generation, editing, dimensions, records and replay are unchanged
there and are never gated on the optional executable.

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
The explicit transforms follow
[ADR 0094](adr/0094-explicit-safe-image-exports-and-contact-sheets.md). Exports have
their own file, `tests/integration/test_image_exports.py` (`-k Export`,
`-k ContactSheet`, also selectable from the workflow file), which runs the real
optional ImageMagick 7 and decodes the resulting pixels in Python; set
`TNY_TEST_MAGICK` to point at an executable that is not on `PATH`. Its
converter-independent cases — validation, aliases, permissions and the
missing-dependency path — run everywhere.
