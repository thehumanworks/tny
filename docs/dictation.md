# Dictation

Talk into the local microphone and turn speech into an editable prompt.
Dictation selects its own provider: ChatGPT/Codex or xAI can transcribe while Grok,
Codex, Grok, or any configured OpenAI-compatible profile runs the conversation.

## Interactive shell

1. Press **Ctrl-R**, or run **`/dictate`** (`/dictate codex` or `/dictate xai` selects explicitly).
2. Speak. The status row shows **Listening**.
3. Press **Enter** or **Ctrl-R** to stop and transcribe.
4. Review or edit the text inserted at the caret. Press **Enter** to send it
   through the current chat provider.

With [normalization](#normalization-and-the-dictionary) enabled the status
row shows **Normalizing** after **Transcribing**; **Esc** or **Ctrl-C** there stops
only the rewrite and inserts the raw transcript.

**Esc** or **Ctrl-C** cancels capture/transcription and preserves the existing
draft. A new approval or clarification also cancels dictation before taking
the keyboard. Typed/pasted bytes during recording or transcription do not
change or submit the draft. Existing text is preserved; spaces separate the
inserted transcript from adjacent words. Normal prompt history and session
persistence start when the user submits, not when transcription completes.
The microphone belongs to the local shell even when `--ssh` routes workspace
tools to another host. Dictation can run while a conversation turn streams;
submitting its finished draft uses the normal steer/queue behavior.

## CLI

```sh
tny dictate                          # Enter stops recording; Ctrl-C cancels
tny dictate --seconds 10              # timed microphone capture
tny dictate --device 'MacBook Pro Microphone'
tny dictate --input-file speech.wav   # file transcription, no recorder needed
tny dictate --input-file speech.wav --json
tny dictate --stt-provider xai --input-file speech.wav
tny --xai-api-key "$XAI_API_KEY" dictate --stt-provider xai --seconds 10
TNY_STT_PROVIDER=xai tny --provider codex  # Ctrl-R uses xAI; chat uses ChatGPT
tny dictate --check --json            # local credentials + recorder check only
tny dictate --input-file speech.wav --check --json  # credentials check only
tny dictate --input-file speech.wav --normalize --json  # dictionary-aware rewrite
tny dictate --no-normalize            # force raw output despite settings/env
```

Plain stdout contains only the transcript plus a newline; progress and errors
go to stderr. With normalization explicitly off, JSON success is one object:

```json
{"kind":"dictate","provider":"codex","text":"Please review these changes."}
```

By default the object also includes the normalization record;
`text` is what plain stdout prints and what the TUI inserts:

```json
{"kind":"dictate","provider":"codex","text":"Ask tny to run kubectl on 23 pods.",
 "raw":"ask tiny to run kube cuddle on twenty three pods","normalized":true,
 "model":"gpt-6-luna","effort":"none","service_tier":null,
 "corrections":[{"span":"tiny","replacement":"tny","reason":"dictionary"}, …]}
```

When the rewrite did not apply, `normalized` is `false`, `text` equals `raw`,
`corrections` is `[]`, and `skipped_reason` says why (see below). `effort` is
the value sent on the last request (`null` when omitted or after an effort
retry); `service_tier` is `"priority"` only when `fast` was sent. With
normalization off the object has only `kind`, `provider`, and `text`, as in the
first example.

`--check` emits `{"kind":"dictate","available":true}` or `false`; it does not
open the microphone or the input file, make requests, or refresh credentials.
Availability means local prerequisites were found, not remote entitlement or
microphone permission. A nonterminal microphone caller must supply
`--seconds N`. Options for the microphone (`--seconds`, `--device`) cannot be
combined with `--input-file`.

To send a transcript directly without composer review, compose the commands:

```sh
tny dictate | tny --provider grok ask --stdin
```

Exit codes: **0** success, **1** configuration/capture/file/protocol error,
**2** HTTP rejection, **130** interruption. No partial transcript is printed
on failure or cancellation. A skipped normalization is not a failure: the raw
transcript is printed, exit status stays 0, and one `normalization skipped`
line goes to stderr. Ctrl-C during **Normalizing** still exits 130 with nothing
on stdout.

## Credentials and provider choice

`--stt-provider NAME` / `/dictate NAME` selects the transcription adapter;
otherwise `TNY_STT_PROVIDER` selects it, defaulting to `codex`.
This never changes `--provider`, model, conversation credentials, or chat URL.
The standalone command does not initialize the conversation profile.

The `codex` adapter uses the existing ChatGPT credential resolution and
refresh: explicit `--chatgpt-token` / `--chatgpt-account-id`, then
`CHATGPT_ACCESS_TOKEN` / `CHATGPT_ACCOUNT_ID`, then `~/.tny/codex-auth.json`,
then `$CODEX_HOME/auth.json` (normally `~/.codex/auth.json`). Sign in with
`tny --provider codex login` or reuse `codex login`.
An OpenAI API key alone does not enable this subscription adapter.

The request is a `file` multipart part named `audio.wav`, with MIME type
`audio/wav`, posted to `https://chatgpt.com/backend-api/transcribe`. The
adapter sends the ChatGPT bearer and account-id headers. Its model is chosen
by that service, separately from the chat model. The trusted
`TNY_CODEX_BASE_URL` gateway override is honored after removing a trailing
`/codex`; the conversation's `base_url` and key are never used for dictation.
HTTP errors show status only, not a response body or bearer token.

### xAI credentials and endpoint

`xai` resolves credentials in this order, independently of chat:

1. The leading global flag `--xai-api-key KEY` (before `dictate`, or when starting the TUI).
2. `XAI_API_KEY`.
3. A named `xai` object in `~/.tny/settings.json`: the variable named by
   `api_key_env`. See the [settings example](settings.md#named-openai-compatible-providers);
   `base_url` configures chat and is not used by this standalone STT adapter.
4. When no explicit key source is configured, the access/session token from
   `~/.grok/auth.json`. Sign in with `tny --provider grok login`.

An explicitly present empty, whitespace-only, CR/LF-bearing, or over-16-KiB
credential is rejected; it does not silently select a lower-priority account.
An explicitly configured missing `api_key_env` fails closed. Stored `api_key`
settings are rejected with migration guidance. No key is
printed or persisted by dictation. Only the existing Grok refresh routine may
update its own auth store, when that login is selected and an actual
transcription starts. Higher-priority credentials never refresh the login.
Standalone dictation reads only user settings and the needed auth store; it
never resolves the conversation profile, creates a session, or contacts chat.

xAI STT is pinned to **`https://api.x.ai/v1/stt`**, using `Authorization:
Bearer <key>` and a single multipart `file` part named `audio.wav` with MIME
`audio/wav`. No ChatGPT account header, Grok chat headers, or chat model is
sent. The REST contract has **no model parameter or versioned model ID**;
the service chooses its default model. The JSON response's `text` string
passes through the shared 64 KiB UTF-8/control-character validation.

The named profile's `base_url`, `XAI_BASE_URL`, `GROK_BASE_URL`, and global
`--base-url` are **ignored for STT**, as are custom auth headers and models.
In particular, a Grok token is never forwarded to an arbitrary configured
STT endpoint. The Grok token fallback uses the requested Bearer format, but
local availability does not establish remote STT entitlement: the official
STT docs specify an xAI API key and do not promise that every Grok subscription
token grants STT access. HTTP rejections report status only, never the body.

`tny dictate --stt-provider xai --input-file speech.wav --check` checks for a
locally usable credential source without opening that file. It makes no
request, refreshes nothing, and opens no microphone. Without `--input-file`,
the existing recorder availability check also applies. Windows and wasm keep
the same file-only behavior; xAI file uploads use the existing HTTP/fetch
transport, with browser CORS restrictions.

## Normalization and the dictionary

STT mangles project vocabulary (`kube cuddle` for `kubectl`, `tiny` for
`tny`), and neither adapter accepts a vocabulary hint. Normalization is a default-on, second request from the **same subscription
credential that just transcribed**, to a small model, followed by a
deterministic check in C
([ADR 0175](adr/0175-dictation-transcript-normalization.md)).

When the selected provider's local credentials and dictation prerequisites are
available, successful dictation normalizes by default. It makes no extra
request when dictation is unavailable. Disable it with `--no-normalize` on
`tny dictate`, `TNY_DICTATION_NORMALIZE=0`, or `~/.tny/settings.json`.
Explicit enablement with `--normalize` or `TNY_DICTATION_NORMALIZE=1` is
also supported:

```json
{ "dictation": { "normalize": { "enabled": true, "timeout_seconds": 20 } } }
```

`"normalize": true` is shorthand. Precedence is `--normalize`/`--no-normalize`,
then `TNY_DICTATION_NORMALIZE` (`1`/`0`), then settings, then on. The TUI
(Ctrl-R, `/dictate`) follows the environment and settings.

| STT adapter | Model (default) | Request |
| --- | --- | --- |
| `codex` | `gpt-6-luna` | ChatGPT credentials and trusted gateway, `POST …/backend-api/codex/responses`, structured output (`text.format`) |
| `xai` with an API key | `grok-4.7` | `POST https://api.x.ai/v1/chat/completions`, `response_format` JSON schema |
| `xai` with a Grok login | `grok-4.7` | `POST https://cli-chat-proxy.grok.com/v1/chat/completions` with the proxy headers and `x-grok-model-override`; JSON requested in the instructions |

Settings `dictation.normalize`:

| Key | Default | Meaning |
| --- | --- | --- |
| `enabled` | `true` | Turn normalization off with `false`; an object without `enabled` inherits the default. |
| `model` | adapter default | String, or `{"codex": …, "xai": …}`. `TNY_DICTATION_NORMALIZE_MODEL` wins. Never the conversation model. |
| `effort` | `off` | Canonical level (`off` → `none`, `light` → `low`) or a provider token; `omit` sends no field. `TNY_DICTATION_NORMALIZE_EFFORT` wins. A rejected value (HTTP 400/422) is retried once without the field. |
| `fast` | `false` | Codex only: `service_tier: "priority"`. Never inherited from the conversation's `--fast`. |
| `timeout_seconds` | `20` | 1–120; one deadline for the whole normalization, retry included. |

The request contains fixed instructions, the transcript and your dictionary
entries, nothing else: no settings, session, workspace files or conversation
history. It does not resolve the conversation profile, start a session or
count as conversation usage.

**Dictionary.** `~/.tny/dictionary.json` (user) merged under
`<workspace>/.tny/dictionary.json` (project, wins per word). Add
`"$schema": "https://raw.githubusercontent.com/thehumanworks/tny/main/schemas/dictionary.schema.json"`
for editor validation.

```json
{
  "tny": { "context": "the agent harness", "aliases": ["tiny", "tee en why"], "case": "exact" },
  "kubectl": { "context": "Kubernetes CLI", "aliases": ["kube cuddle"] },
  "Jev": { "context": "decision engine", "aliases": ["jeff"], "case": "exact" },
  "Postgres": "the database"
}
```

A word is 1–64 bytes with a letter or digit and no surrounding space; a
context at most 256 bytes; at most 8 aliases per word, 256 words and 64 KiB
per file; no control characters, duplicate words or unknown keys. `case:
"exact"` requires the replacement to match byte for byte; otherwise ASCII case
is ignored. Aliases are known mishearings: a correction whose span has more
than one word is accepted only when it matches one. A missing file is empty;
an invalid one skips normalization.

**What can change.** The model returns the rewritten text plus every change as
`span` → `replacement` with a reason. C accepts the rewrite only if every
correction is admissible for its reason (`dictionary` — a dictionary word;
`case` — letter case only; `punctuation` — punctuation only, digits' inner
punctuation included, so `3.5` never becomes `35`; `number` — an English
cardinal below 10^12 as digits, `twenty three` → `23`, or regrouped digits),
the listed changes reproduce the text exactly, the text is a valid transcript,
there are at most 64 corrections, and the rewrite changes at most `3 + n/4` of
the transcript's `n` words. Spoken punctuation, decimals, ordinals and years
are left alone.

**Fail open.** Anything else keeps the raw transcript, exactly as without
normalization, with one `skipped_reason`: `cancelled`, `invalid_config`,
`dictionary_invalid`, `no_credential`, `transport`, `timeout`, `http_<status>`,
`effort_rejected`, `provider_error`, `malformed_output`, `malformed_stream`,
`oversized_output`, `incomplete_output`, `out_of_memory`, or
`rejected:<verdict>` (`invalid_text`, `too_many`, `empty_span`,
`inadmissible`, `unlisted`, `edit_bound`). Provider bodies are never shown.

The verifier and the lifecycle are specified and proved in Lean 4
(`tests/formal/dictation`); the C unit suite replays the proven definitions'
golden tables. Live effort probing and the fast-tier benchmark are recorded in
ADR 0175 once run with authorized accounts.

## Capture, limits, and platforms

| Platform | Microphone | WAV file |
| --- | --- | --- |
| macOS | FFmpeg with AVFoundation, audio input only | Works |
| Linux | `arecord` (ALSA), otherwise FFmpeg with PulseAudio | Works |
| Windows / MSYS / Cygwin | Clean unavailable error | Works through existing HTTP transport |
| Node/browser wasm | Clean unavailable error | Remote-only, through existing fetch transport; CORS applies (normalization too) |

Recorders are optional host programs found on `PATH`, launched only on an
explicit dictation action. No audio library is bundled or loaded at startup.
`--device` or `TNY_AUDIO_DEVICE` selects the device, defaulting to `default`.
The device is passed as one literal process argument, never shell text. On
macOS allow microphone access for the launching terminal/application in
System Settings if prompted; FFmpeg must include AVFoundation input support.

Capture produces mono PCM16 little-endian at 24 kHz. Microphone audio stays
in memory and is discarded on success, failure, or cancellation. Nothing is
written to a recording file or session. External recorder stderr is hidden
from the terminal. Stop gives the owned recorder process group one second to
exit, then kills/reaps it if necessary; cancellation kills/reaps immediately.

Clips must last **1–300 seconds** and be at most **25 MiB**. `--seconds N`
counts captured samples, with ten seconds of startup headroom in the wall
timeout. File input must be a complete RIFF WAV, PCM16, mono/stereo, at
8–96 kHz; convert other formats before passing them. Transcripts are bounded
to **64 KiB**, validated as UTF-8, and reject terminal control characters and
empty/whitespace-only results. The request has a 60-second deadline. The
existing HTTP seam performs bounded synchronous connect/upload; response
reads and recording use the ordinary event loop and remain cancellable.

## Extending

`core/dictation.h` is the common lifecycle/result interface. `core/dictation.c`
owns capture, WAV/text validation, limits, cancellation, and cleanup.
`util/audio_capture.c` is the host OS seam for recorders. CLI and TUI files
only drive that lifecycle and display or insert the result.

Add an adapter implementing `core/dictation_provider.h`: local availability,
start from WAV bytes, poll fd, incremental response step, and destroy. Register
it in `core/dictation.c`. Adapters own their own authentication and wire
formats; no display or conversation-provider changes are needed. There is no
agent microphone tool and no expansion of the public libtny ABI.

Adapters that support normalization also set `normalize_model` and
`normalize_target`, which returns the endpoint, headers and wire for the same
credential they transcribe with. `core/dictation_verify.c` (verifier and
lifecycle), `core/dictation_dictionary.c` and `core/dictation_normalize.c`
(configuration and the streamed rewrite request) are adapter-neutral.

Run `make test-dictation` for the unit and local HTTP/recorder fixtures,
including the normalizer mocks for both adapters, and
`make verify-dictation-proofs` (needs `lake`) for the Lean proofs and their
golden tables.
`make wasm wasm-dictation-fixture` followed by
`TNY="$PWD/build/wasm/tny" python3 tests/integration/test_dictation.py` exercises
file transcription and normalization through wasm. The separate fixture
executable accepts only loopback test URLs (`TNY_DICTATION_FIXTURE_URL`,
`TNY_DICTATION_FIXTURE_NORMALIZE_URL`); production binaries have no xAI STT or
normalizer URL override.
All fixture credentials and audio are synthetic.

See [ADR 0079](adr/0079-provider-independent-dictation.md) and
[ADR 0175](adr/0175-dictation-transcript-normalization.md).
