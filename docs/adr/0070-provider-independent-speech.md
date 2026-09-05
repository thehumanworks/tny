# 0070 — Provider-independent speech using the Codex login

Date: 2026-09-05
Status: accepted

## Context

Agents need to vocalise a message without asking the user to manage audio
files. The supplied Python TTS client already demonstrates ChatGPT's
`/backend-api/pronunciation/synthesize?format=mp3` protocol. Its environment
credential plumbing must not become a dependency on Doppler or another
credential service. The active chat model should not determine access to speech.

## Decision

- Introduce the C11 `tny_speech_request` service and a small provider table with
  capability and MP3-synthesis callbacks. The first adapter is `codex`, using
  OpenAI's ChatGPT pronunciation service. Future adapters implement this
  boundary without changing CLI, tool, playback or export logic.
- Reuse ADR 0066's credential precedence and native refresh. Require an OAuth
  access token and account ID; API-key-only auth does not qualify. Resolve
  credentials from their original sources rather than copying the current
  chat profile's key or headers. Only the existing trusted
  `TNY_CODEX_BASE_URL` override can redirect this adapter.
- Ship `cove`, `en-US`, speed 1.0 and MP3 as defaults. Support voice override,
  raw MP3 and JSON/base64 responses, using the existing HTTP/JSON/codecs.
  Bound text to 16 KiB, wire response to 16 MiB and waits to fixed deadlines.
  Do not retry a rejected synthesis automatically or print response bodies.
- CLI text is stdin-only under ADR 0064. `--json` changes only the result
  format because speech has no structured input form. `--tts-provider`
  selects speech independently of global `--provider`; the command bypasses
  chat configuration and starts no chat backend.
- Capability checks are local and do not refresh tokens or call a provider.
  Gate native tool schema and shell guidance on credentials plus player.
  Always recheck before executing. Host-owned tools remain host-owned:
  Cursor/ACP and external agents can call the standalone CLI and `--check`.
- Add `speak` to the full tool profile, with text and voice only. Shell
  profiles keep their existing tool count and use the narrow in-process
  first-party interception from ADR 0063. Speech uses its own permission
  identity and is not classified as read-only. Embedded runtimes hide it.

## Consequences and verification

No new linked library, Python dependency or auth provider. A locally present
login does not prove a remote entitlement or non-revoked token; provider errors
remain runtime failures. This is the supplied ChatGPT pronunciation contract,
not a claim of compatibility with OpenAI's public audio/speech endpoint.

`tests/test_speech.c` covers capability, schema, dispatch, permissions and
interception; `tests/integration/test_speech.py` covers credentials/refresh,
raw/base64/chunked transport, malformed/truncated/oversized replies, atomic
export, cancellation, and a non-Codex conversation invoking speech with separate
credentials. Live synthesis and default-voice playback were verified with the
user's existing account on macOS on 2026-09-05; no credential is stored here.

The targeted mutation pass detected both compilable text-boundary mutants
(one unit kill and one integration kill); seven logical-operator mutations
were rejected by the strict compiler. The wasm CI job also runs the speech
export fixtures and checks unavailable playback.
