# Dictation

Talk into the local microphone and turn speech into an editable prompt.
Dictation selects its own provider: ChatGPT/Codex or xAI can transcribe while Grok,
Claude, Cursor, ACP, or any OpenAI-compatible provider runs the conversation.

## Interactive shell

1. Press **Ctrl-R**, or run **`/dictate`** (`/dictate codex` or `/dictate xai` selects explicitly).
2. Speak. The status row shows **Listening**.
3. Press **Enter** or **Ctrl-R** to stop and transcribe.
4. Review or edit the text inserted at the caret. Press **Enter** to send it
   through the current chat provider.

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
TNY_STT_PROVIDER=xai tny --provider claude  # Ctrl-R uses xAI; chat stays Claude
tny dictate --check --json            # local credentials + recorder check only
tny dictate --input-file speech.wav --check --json  # credentials check only
```

Plain stdout contains only the transcript plus a newline; progress and errors
go to stderr. JSON success is one object:

```json
{"kind":"dictate","provider":"codex","text":"Please review these changes."}
```

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
on failure or cancellation.

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
   `api_key_env`, then stored `api_key` if that variable is unset. The object
   must have a nonempty `base_url`, following the generic named-provider
   convention; see the [settings example](settings.md#named-openai-compatible-providers).
4. The access/session token resolved by the existing Grok auth reader from
   `~/.grok/auth.json`. Sign in with `tny --provider grok login`.

An explicitly present empty, whitespace-only, CR/LF-bearing, or over-16-KiB
credential is rejected; it does not silently select a lower-priority account.
An unset settings `api_key_env` may fall back to stored `api_key`. No key is
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

## Capture, limits, and platforms

| Platform | Microphone | WAV file |
| --- | --- | --- |
| macOS | FFmpeg with AVFoundation, audio input only | Works |
| Linux | `arecord` (ALSA), otherwise FFmpeg with PulseAudio | Works |
| Windows / MSYS / Cygwin | Clean unavailable error | Works through existing HTTP transport |
| Node/browser wasm | Clean unavailable error | Remote-only, through existing fetch transport; CORS applies |

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

Run `make test-dictation` for the unit and local HTTP/recorder fixtures.
`make wasm wasm-dictation-fixture` followed by
`TNY="$PWD/build/wasm/tny" python3 tests/integration/test_dictation.py` exercises
file transcription through wasm. The separate fixture executable accepts
only a loopback test URL; production binaries have no xAI STT URL override.
All fixture credentials and audio are synthetic.

See [ADR 0079](adr/0079-provider-independent-dictation.md).
