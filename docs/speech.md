# Speech

`tny speak` vocalises a message using your existing ChatGPT subscription login.
It is independent of the provider running the conversation: OpenAI, Claude,
Grok, Codex, Cursor and ACP agents can invoke the CLI through their shell.
No Python, Codex subprocess, API key, Doppler or additional auth service is
required. API-key-only Codex logins do not grant ChatGPT speech access.

```sh
printf 'The tests passed.' | tny speak
printf 'The tests passed.' | tny speak --voice glimmer
printf 'The tests passed.' | tny speak --output-file message.mp3
printf 'The tests passed.' | tny speak --json
tny speak --check --json
```

Text is plain UTF-8 on stdin, including in `--json` mode, with a 16 KiB limit.
Whitespace-only input and embedded NUL are errors. Text is not accepted on argv.
The default speech provider is `codex` (`--tts-provider codex`); leading
`--provider` and the selected chat profile do not select or configure speech.
The initial provider uses the ChatGPT pronunciation endpoint with voice `cove`,
language `en-US`, speed `1.0`, and MP3 format. `--voice NAME` overrides the voice.
This endpoint follows the supplied working pronunciation client; it is not the
public API-key-based OpenAI audio/speech API and may change independently.

Credential precedence and refresh are shared with [Codex](backends/codex.md):
leading `--chatgpt-token` / `--chatgpt-account-id`, `CHATGPT_ACCESS_TOKEN` /
`CHATGPT_ACCOUNT_ID`, tny's login store, then `$CODEX_HOME/auth.json`.
Account ID can come from the token's JWT claim. Run `tny --provider codex login`
if needed. Store credentials refresh before synthesis; flag and env tokens
remain caller-managed. `TNY_CODEX_BASE_URL` is the same trusted gateway/test
setting as Codex chat and must end in `/backend-api` or `/backend-api/codex`.
Speech never sends the credential to the active conversation's base URL.

## Playback and export

Playback is automatic and synchronous. macOS uses `afplay` when present;
otherwise tny tries `ffplay`, `mpv`, then `mpg123` from PATH. Linux uses the same
fallbacks. No decoder is bundled and no player is installed automatically.
The selected program receives fixed arguments and an anonymous, seekable
0600 temporary input: its pathname is unlinked before any audio is written.
The descriptor closes after playback, including player failure or interruption.
No MP3 is added to the workspace or session. Text/tool calls still follow the
normal conversation transcript policy.

`--output-file PATH` exports instead of playing. The parent directory must
exist. A private sibling temporary file is renamed onto PATH after successful
synthesis and writing; a failed response leaves existing output unchanged.
An existing symlink at PATH is replaced, not followed. Playback is not required
for export. Relative output paths use the command's current directory.

Playback occurs on the machine running tny, including when the workspace uses
`--ssh`; it is not streamed to a remote frontend. A headless host may have a
player installed without a usable audio device; that is reported at playback.
Windows and wasm support export through the shared service but return a clean
unavailable error for playback. Browser export uses its virtual filesystem and
requires endpoint CORS support; no browser audio bridge is introduced.

## Agents and results

The native `all` tool profile advertises `speak` with `text` and optional `voice`
only when a ChatGPT access token, account ID and local player resolve. Embedded
runtimes do not expose this host side effect. Shell profiles receive the CLI
recipe under the same condition. Simple `printf … | tny speak [--voice NAME]
[--json]` and quoted heredoc forms run in-process with the `speak` permission
identity. Other command forms retain ordinary terminal permissions. Speech
uses the existing permission engine; it is not a read-only safe tool. Host
agents retain their own tool ownership and can inspect `tny speak --check`.

`--check` checks local capability without network traffic or token refresh.
With `--output-file PATH`, it checks synthesis credentials without requiring a
player. It cannot prove server entitlement, token revocation or audio hardware.
A 401/403 gives a login diagnostic and no audio is played.

Successful human playback/export is silent on stdout. `--json` returns one
object: `{"kind":"speak","ok":true,"played":true}` for playback, or
`played:false` for export. Check returns `{"kind":"speak","available":true}`
(or false). Diagnostics go to stderr and never include provider response bodies
or credentials. Exit codes are 0 success, 1 input/configuration/I/O failure,
2 HTTP rejection, and 130 interruption. Intercepted calls preserve the same exit status. Responses are bounded to
16 MiB, headers/body each have a 60-second deadline, and playback has a
five-minute ceiling. Connection and credential refresh use existing timeouts.

See [ADR 0070](adr/0070-provider-independent-speech.md) and
[ADR 0071](adr/0071-ephemeral-host-audio-playback.md).
