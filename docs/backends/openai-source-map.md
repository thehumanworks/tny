# Native provider source map

Start with the seam that owns the behavior rather than reading the entire
provider loop. All paths below are private implementation, not the public
`include/tny` ABI. Responses and Chat Completions share the same loop.

| Change | Start here | Boundary |
| --- | --- | --- |
| Turn transitions, request orchestration, retry scheduling, tool execution, cancellation or checkpoints | `src/backends/openai/openai.c` | Owns `oa_impl`, event ordering and when side effects occur. Keep these transitions together. |
| Error categories, retry eligibility, bounded/redacted diagnostics or recovery messages | `src/backends/openai/errors.c`, `errors.h` | Allocation-free policy over JSON and explicit route flags. No connection, runtime or session access. |
| Responses input, tools or structured-output translation | `src/backends/openai/responses.cpp` | Converts persistent chat-shaped messages into request JSON. |
| SSE event normalization, reasoning fragments or terminal events | `src/backends/openai/stream_decode.cpp`, `stream_decode.h` | Owns decoded data and emits borrowed synchronous events. HTTP/SSE framing remains in `src/net/`. |
| Incremental tool-call identity and argument assembly | `src/backends/openai/toolcalls.cpp`, `toolcalls.h` | Owns fragments; no execution or permission decisions. |
| Request scratch/JSON/header lifetime and connection storage | `src/backends/openai/request_owner.cpp`, `request_owner.h` | Private C++ ownership. The C loop decides when to send, retry or drop. |
| Turn buffers and pending permission/custom-tool records | `src/backends/openai/turn_owner.cpp`, `turn_owner.h` | Private C++ storage. The loop retains cancellation and settlement decisions. |
| Profile selection or credentials | `src/core/config.c`, `provider_extras.c`, `openai_auth.c`, `codex_auth.c`, `grok_auth.c` | Configuration/authentication outside protocol parsing. |

## Failure-policy seam

`oa_error_classify` accepts the error object/string after envelope decoding.
The result owns its bounded fields; recognized ChatGPT-plan codes point to
static allowlisted spellings, never retained JSON. The caller owns the optional
debug-detail buffer and resets it at the same request boundary as before.
`oa_error_format` receives explicit plan/proxy flags and only receives raw
provider detail for a final diagnostic when debug output was enabled.

`openai.c` still owns retry budget/backoff, continuation, connection teardown,
authentication event codes and terminal delivery. Classification does not
schedule retries. The extraction keeps existing API-key, ChatGPT-plan and Grok
proxy diagnostics, including plan-code precedence over HTTP status.

The module is shared by native and wasm builds; it performs no OS or network
operations. No new platform seam or public ABI is introduced.

## Focused verification

- `make test-openai-errors`: links only error policy plus JSON/util/allocator
  code. Runs classification precedence, static plan-code lifetime, privacy,
  UTF-8/control-character bounds, route-specific text and Retry-After cases,
  using the normal debug sanitizer settings. No provider or CPython build.
- `make debug` then `build/tny-test -s openai`: runs both the error-policy suite
  and backend unit suite, including decoder and ownership behavior.
- `tests/integration/test_openai.py` and
  `tests/integration/test_openai_signin.py`: loopback provider lifecycle,
  retries and plan-route regressions against a built binary. Run the complete
  `make test` gate before claiming end-to-end provider verification.

Transport split-boundary coverage remains with the existing SSE/HTTP and
stream-decoder suites; error policy consumes already decoded JSON rather than
reimplementing framing.
