# ADR 0185: Grok proxy client-version compatibility

Status: accepted. Date: 2026-10-07.

## Context

The Grok subscription proxy rejects tny's `x-grok-client-version: 0.1.202`
with HTTP 426. Authenticated `POST /v1/chat/completions` probes with an empty
JSON object isolate the version gate without requesting inference:

| Client version | HTTP response | Meaning |
| --- | --- | --- |
| `0.1.202` | 426 | The proxy requires Grok CLI `1.0.13` or later |
| `1.0.45` | 400 | Version accepted; the empty messages are rejected |

Both requests use HTTP/1.1 and receive no `Upgrade` header. Authenticated
`GET /v1/models` succeeds with either version, so catalog access alone does
not establish inference-route compatibility.

The [official proxy header implementation](https://github.com/xai-org/grok-build/blob/2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8/crates/codegen/xai-grok-shell/src/agent/proxy_headers.rs)
documents `x-grok-client-version` as the version-gate input. Its shared
[version crate](https://github.com/xai-org/grok-build/blob/2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8/crates/codegen/xai-grok-version/Cargo.toml)
pins `1.0.45` at commit `2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8`.

## Decision

Update the shared subscription compatibility version to `1.0.45`. The native
agent profile and xAI dictation normalizer use the same constant. Retain
`TNY_GROK_CLIENT_VERSION` for future rolling minimum changes without a rebuild.
Keep HTTP/1.1, Chat Completions and SSE; this failure requires no HTTP/2 or
WebSocket implementation. Public xAI API-key requests retain their wire and
receive no subscription version header.

For a Grok subscription HTTP 426, report that a newer client version is
required and identify updating tny or setting `TNY_GROK_CLIENT_VERSION` as
the recovery. The response remains permanent and is never retried. Provider
messages remain hidden unless `TNY_DEBUG_PROVIDER_ERRORS=1` is set; unrelated
profiles and public xAI errors keep their generic diagnostics.

## Verification

The native profile fixture models the observed `1.0.13` minimum over
HTTP/1.1. It covers a complete tool round with the default pin, an explicit
newer override, and an old override returning one terminal 426 with an
actionable diagnostic. Unit checks cover the default, override and public
API header separation; the dictation fixture checks the same version.

An explicitly authorized live check of the rebuilt native CLI in an empty
temporary workspace returned `GROK_OK` from `grok-4.6`: exit 0, one step,
zero tool calls and no stderr. The same HTTP/1.1 transport and Chat Completions
stream work with the updated version; no additional proxy headers were needed.

Local macOS `make test`, `make quality` and `make leaks` passed. The unit
suite recorded 630 passes and one skip; integration groups completed with
their declared platform skips. Browser-only WASM acceptance remains a
separate check.

Wasm uses the shared profile and decoder through browser fetch. The version
header is unchanged in name; the proxy must permit it through CORS for browser
use. Local fixtures do not establish live model entitlement or paid inference.
