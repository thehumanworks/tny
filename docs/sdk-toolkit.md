# Standalone SDK toolkit

Python's `Toolkit` / `AsyncToolkit` and TypeScript's `Toolkit` expose the native
image, speech, transcription, and prompt optimisation services directly. They
require **libtny ABI 1.2+** on the existing native SDK platforms: macOS arm64 and
Linux glibc x86_64/aarch64. They need no `tny` executable, agent `Runtime`,
session, or disposal call. Each method releases its native operation before
returning or raising.

| Operation | Python | TypeScript | Result |
| --- | --- | --- | --- |
| Generate image | `generate_image(prompt, output_file=...)` | `generateImage(prompt, {outputFile})` | Path, MIME type, byte count, provider, model |
| Edit image | `edit_image(prompt, images=[...], output_file=...)` | `editImage(prompt, {images, outputFile})` | Same image result; one to five references |
| Text to speech | `speak(text, output_file=...)` | `speak(text, {outputFile})` | MP3 path or `None`/`null`, played flag, provider, voice, MIME type |
| File to text | `transcribe(input_file)` | `transcribe(inputFile)` | Transcript and provider |
| Microphone to text | `dictate(seconds=...)` | `dictate({seconds})` | Transcript and provider |
| Prompt optimisation | `optimise(text, ...)` | `optimise(text, options)` | Improved prompt, provider, model |

Both languages also accept `optimize`. `AsyncToolkit` has the same Python
methods and options, awaited. Python transcript/prompt text is `bytes`, matching
the existing SDK's explicit decoding contract; metadata is `str`, and paths
are `pathlib.Path`. TypeScript results contain strings and are frozen. Image
exports require an output path. Omit the speech output path to play locally.
Capture requires an explicit integer duration of 1–300 seconds.

```python
from tny import Toolkit, ToolkitConfig

kit = Toolkit(ToolkitConfig(workspace="/path/to/project"))
image = kit.generate_image("A small tree", output_file="tree.png")
kit.edit_image("Make the leaves blue", images=[image.path], output_file="blue.png")
kit.speak("Your image is ready", output_file="message.mp3")
transcript = kit.transcribe("recording.wav")
draft = kit.optimise(transcript.text)
print(draft.text.decode("utf-8"))
```

```typescript
import { Toolkit } from "@thehumanworks/tny";

const kit = new Toolkit({ workspace: "/path/to/project" });
const image = await kit.generateImage("A small tree", { outputFile: "tree.png" });
await kit.editImage("Make the leaves blue", {
  images: [image.path], outputFile: "blue.png",
});
await kit.speak("Your image is ready", { outputFile: "message.mp3" });
const transcript = await kit.transcribe("recording.wav");
const draft = await kit.optimise(transcript.text);
console.log(draft.text);
```

## Configuration and authority

The constructor captures its workspace (default: current directory). Relative
input, output, reference, and settings paths resolve there; calls never change
the process directory. Concurrent calls have independent native contexts.
Callers must coordinate writes to the same output path.

Images and speech use `codex`. Transcription/capture support `codex` and `xai`;
omission follows `TNY_STT_PROVIDER`, then `codex`. Every call accepts a `provider`
override. Image calls also accept `model`, `quality` (`auto`, `low`, `medium`,
`high`, `xhigh`, `max`), and `size`. Speech accepts `voice`; capture accepts
`device`. Defaults are those of the shared CLI services.

`ToolkitConfig` accepts `chatgpt_token`, `chatgpt_account_id`, `codex_base_url`,
and `xai_api_key`. TypeScript uses `chatgptToken`, `chatgptAccountId`,
`codexBaseUrl`, and `xaiApiKey`. Explicit credentials take precedence over the
existing environment/login resolution; empty or invalid explicit credentials
are errors. A Codex token can supply its account id through its JWT claim.
The Codex gateway override has the same shape as `TNY_CODEX_BASE_URL` (normally
ending in `/backend-api/codex`). It never inherits an optimisation URL/key.
xAI STT retains its fixed native `https://api.x.ai/v1/stt` endpoint.

The toolkit reads `~/.tny/settings.json` for profiles and optimisation defaults.
Set `settings_path` / `settingsPath` to use an explicit file; missing or
unparseable explicit settings fail before provider I/O. An empty `{}` file
isolates settings. Provider environment variables and login stores remain
available; supply explicit credentials for account isolation. The toolkit
never writes credentials into settings.

Optimisation accepts `provider`, `model`, `base_url`, `api_key`, `wire_api`
(`chat` or `responses`), and `timeout_seconds` (integer 1–86400). TypeScript uses
`baseUrl`, `apiKey`, `wireApi`, and `timeoutSeconds`. These override the resolved
profile for that call. Omission uses native environment/user settings defaults,
including the 300-second timeout. The optimiser reads relevant project files
but never submits the draft, persists an agent transcript, or enables writes,
shell commands, extensions, MCP, or delegation. The caller owns review and later
submission of the returned prompt.

## Cancellation, results, and limits

Python sync calls accept `cancellation=CancellationToken()`, cancellable from
another thread. `AsyncToolkit` also accepts a token; cancelling its asyncio
task signals native work and joins cleanup, including repeated cancellation.
TypeScript accepts `signal: AbortSignal` on every call. Abort rejection and Node
worker termination wait for native cleanup. Connect/TLS setup can take up to
its existing 15-second deadline; cancellation is cooperative.

Image/speech exports retain atomic replacement: failure/cancellation before
commit preserves existing output. A completed commit wins a late cancellation.
Capture/playback start only when explicitly invoked and require the same host
recorder/player as the CLI. File transcription/export need no audio hardware.
Returned artifact paths are caller-owned and are not automatically deleted.

Shared limits apply: image prompts/speech text up to 16 KiB; one to five edit
references up to 8 MiB each; image output up to 32 MiB; MP3 output up to 16 MiB;
PCM16 WAV input up to 25 MiB and 1–300 seconds (mono/stereo, 8–96 kHz); and
transcript/optimisation text up to 64 KiB. Validation fails before provider I/O.
Errors use existing `TnyError` categories and omit credentials, prompts, and
provider response bodies. Python result reprs omit transcript/prompt text.
Native request strings and staging copies are wiped on release; language-owned
immutable strings cannot promise wiping.

No wasm/browser SDK is introduced. Existing wasm CLI services retain their
documented file transcription/export behavior and clean host-audio errors.

## C ABI envelope (version 1)

```json
{
  "version": 1,
  "operation": "generate_image",
  "config": {"workspace": "/path/to/project"},
  "request": {"prompt": "A small tree", "output_file": "tree.png"}
}
```

The case-sensitive schema rejects unknown/duplicate keys, nulls, invalid UTF-8
or NUL strings, wrong types, and envelopes above 256 KiB. Optional fields are
omitted. `config.workspace` is required and absolute; other config fields use
the Python names above. Request fields are:

| Operation | Required | Optional |
| --- | --- | --- |
| `generate_image` | `prompt`, `output_file` | `provider`, `model`, `quality`, `size` |
| `edit_image` | `prompt`, `output_file`, `images` | `provider`, `model`, `quality`, `size` |
| `speak` | `text` | `provider`, `voice`, `output_file` |
| `transcribe` | `input_file` | `provider` |
| `dictate` | `seconds` | `provider`, `device` |
| `optimise` | `text` | `provider`, `model`, `base_url`, `api_key`, `wire_api`, `timeout_seconds` |

`tny_toolkit_job_create` copies/validates without I/O; `tny_toolkit_job_run`
executes once on a caller-selected thread. `tny_toolkit_job_cancel` is a sticky
atomic request, including before run. `tny_toolkit_job_result` borrows JSON
after success until `tny_toolkit_job_destroy`; errors have no result. Destroy
refuses running work with `TNY_STATUS_BUSY`. Join run and stop cancellation
callers before destroying. Calls except cancellation must be serialized on
each job. Handles reject use after fork.

Image JSON retains native fields `kind`, `ok`, `operation`, `provider`, `model`,
`path`, `mime_type`, and `bytes`. Speech returns `provider`, `voice`, `mime_type`,
`path` (null for playback), and `played`. Transcription returns `provider` and
`text`; optimisation adds `model`. SDKs copy results before destroying the job.
