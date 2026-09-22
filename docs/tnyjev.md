# tnyjev: typed Jev decisions

**tnyjev** is tny's isolated TypeSafe AI Jev decision client. It powers two
standalone toolkit commands: `tny score` and `tny choose`. It is independent
of the conversation provider and does not start an agent turn or session.
See [ADR 0165](adr/0165-tnyjev-decision-engine.md).

This first integration does **not** change reasoning effort, model routing,
permissions, memory, or fear/excitement reactions. It returns a decision for
caller code to consume. `choose` does not execute the selected route.

## Credentials and configuration

Obtain a key from the [TypeSafe console](https://console.typesafe.ai/) and set
`TYPESAFE_API_KEY` in your environment. Do not put keys in repository files,
command arguments, or project settings. Missing or empty credentials produce
a clear error without an API request. Help works without credentials.

| Setting | Meaning |
| --- | --- |
| `TYPESAFE_API_KEY` | Required independent Bearer credential; no chat credential fallback |
| `TNY_JEV_MODEL` | Jev model ID; default `jev-latest` |
| `--model MODEL` | Overrides the model environment variable; a leading global `--model` also works |
| `TNY_JEV_URL` | Full endpoint override; default `https://api.typesafe.ai/v1/systemone` |
| `--timeout SECONDS` | Response budget, integer 1–300 seconds; default 60 |

Model IDs are passed through, so a versioned model such as `jev-1.13.0` can
be pinned without a tny release. JSON output reports the returned model ID,
which can differ from the requested alias. This model is not registered as
a conversational backend and does not appear in the chat model picker.

Endpoint overrides require HTTPS, except HTTP to `127.0.0.1` or `localhost`
for local fixtures/proxies. URLs with user info, query strings or fragments
are rejected. An override sends the supplied credential and state to that
endpoint: use only a trusted endpoint. There is no redirect following.

Connection and write operations use the shared transport's own deadlines.
`--timeout` starts after the request is written and bounds response headers
and body together; it is **not** an end-to-end connection deadline.

## `tny score`: yes/no probability

```sh
tny score 'Is this urgent?' --state 'Production is down'
printf '%s' 'The task only renames a variable' |
  tny score 'Does this task need extended reasoning?' --json
cat state.json | tny score --stdin-json 'Is the task complete?'
```

Plain stdout contains only a number in `[0,1]`, followed by a newline: zero
means no, one means yes. No threshold is applied. Decide any threshold in
your own caller. Values are model estimates, not correctness guarantees.

**Important naming difference:** TypeSafe calls this primitive **Noul**,
which returns `P(yes)`. Jev also has an ordinal rubric primitive named
**Score**; it is not a yes/no probability and is not exposed here.

Example `--json` output (illustrative, not a live inference result):

```json
{"kind":"score","primitive":"noul","score":0.75,"model":"jev-1.13.0","usage":{"input_tokens":12,"output_tokens":3}}
```

Noul has no separate confidence field. The CLI does not invent one.

## `tny choose`: typed router

```sh
printf '%s' 'Refund my order' | tny choose \
  --choices '{"billing":"Payments, invoices, refunds","support":"Bugs and outages"}'

cat state.json | tny choose --stdin-json --json \
  --choices '{"fast":{"use":"Simple tasks"},"reasoning":{"use":"Complex tasks"}}' \
  'Which model class best fits the task?'
```

`--choices` takes a JSON **object**, not an array. Each unique, nonempty key
is a route name. Descriptions may be strings, objects, arrays, or `null`.
There must be 1–255 routes. Descriptions are transmitted as structured JSON,
not flattened into a prompt. Duplicate keys, numeric/boolean descriptions,
embedded NULs in keys, and control characters in keys are rejected.

The optional question defaults to `Which option best matches the state?`.
Write an explicit question when routing criteria need more guidance.
Plain stdout contains only the chosen key and a newline. JSON adds the full
probability distribution, confidence, returned model, and usage:

```json
{"kind":"choose","primitive":"choice","choice":"billing","confidence":0.8,"probabilities":{"billing":0.75,"support":0.25},"model":"jev-1.13.0","usage":{"input_tokens":12,"output_tokens":3}}
```

The chosen key must be present in the supplied routes. Every supplied route
must have exactly one finite probability in `[0,1]`; the sum must be within
`0.00001` of one. The selected route must have the highest probability
(ties allowed, numeric comparison tolerance `1e-9`). Confidence must also be
in `[0,1]`. Invalid results fail closed, without stdout or a fallback route.

## State input and CLI errors

Use **one** state source:

- `--state TEXT`: literal UTF-8 text. JSON-looking text remains a string.
- `--state-json JSON`: parse a JSON string, object, or array.
- `--stdin`: read literal UTF-8 text to EOF.
- `--stdin-json`: read a JSON string, object, or array to EOF.
- With no state flag, redirected stdin is read as text. A terminal is not
  prompted; supply state explicitly or pipe it.

The score question is required. Quote it as one argument. Use `--` before a
question that starts with `-`. JSON top-level numbers, booleans and null are
not valid state. Input must be nonempty and free of literal NUL bytes.
Each input, complete serialized request, and response is bounded to 1 MiB;
JSON escaping can make the request larger than the input. Route keys are
bounded to 1024 UTF-8 bytes, model IDs to 255, credentials to 8192 printable
ASCII bytes. Oversized or invalid inputs are rejected before HTTP.

Both commands accept `--json` either before or after the command. Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | Valid result successfully written, or help displayed |
| 1 | Usage, configuration, authentication, HTTP, protocol, timeout, or output error |
| 130 | Observed SIGINT/SIGTERM interruption |

Diagnostics go to stderr. They do not echo credentials, state, endpoint URLs,
or provider error bodies. HTTP errors include status codes; 401/403 suggest
checking the key, and 429/529 suggest retrying later. There are **no automatic
retries**, including on rate limits or overload. Callers own retry/backoff
and confidence policy. Cancellation is checked between transport operations
and at most every 50 ms while waiting for response readiness; an in-progress
connect/write remains subject to the shared transport's deadlines.

## Typed module and ownership

Files:

- `src/core/tnyjev.h`: C11 API usable from C and C++; typed configuration,
  request, tagged text/JSON value, route, status, and result types.
- `src/core/tnyjev.c`: validation, serialization, response decoding, bounded
  HTTP collection, cancellation, and cleanup.
- `src/core/tnyjev_internal.h`: private encode/decode seam for offline tests.
- `src/cli/cmd_jev.c`: argv/stdin, environment lookup, signals, output.

The module depends only on shared JSON, buffer, transport and poll utilities.
It has no `tny_ctx`, session, settings, environment lookup, global mutable
client state, chat provider, or SDK dependency. C++ is not required for this
small client; its public header has an `extern "C"` boundary. It is compiled
by the existing shared source graph, not shipped as a separate library or
exported as a stable libtny ABI. Python/TypeScript toolkit wrappers and native
model-call tool registrations are not part of this first increment.

A C caller supplies credentials and state explicitly:

```c
#include "core/tnyjev.h"

/* key and state are supplied by the caller, not read from the environment. */
tnyjev_config config = {.api_key = key};
tnyjev_request request = {
    .kind = TNYJEV_SCORE,
    .instructions = "Does this task need extended reasoning?",
    .state = {TNYJEV_TEXT, state},
};
tnyjev_result result;
char error[256];
tnyjev_status status = tnyjev_evaluate(&config, &request, &result,
                                      error, sizeof error);
if (status == TNYJEV_OK) {
    double yes_probability = result.value.score;
    /* Caller policy consumes yes_probability. */
}
```

All request/config strings and arrays are borrowed for the synchronous call.
Result storage is caller-owned, contains no pointers, and needs no destructor.
For Choice, `choice_index` and the probability array follow **request order**,
not response object order. The union tag equals the requested kind. Results
are published only after complete validation; failure zeros the result.
`tnyjev_status` distinguishes invalid input, missing/unsafe credentials,
transport, HTTP, protocol, timeout, cancellation and allocation failures.
The optional error buffer can be NULL. Buffer allocation failures cannot
produce a partial decision. Request, credential header and response buffers
are cleared before release; no session/transcript stores these calls.

Wire requests use `POST /v1/systemone`, Bearer auth, and one question named
`decision` in the `questions` map. Responses must contain the matching typed
answer, a model string, and nonnegative integer input/output token counts.
Duplicate typed fields and malformed/truncated JSON are rejected; unrelated
unknown fields are ignored for forward compatibility. Shared HTTP machinery
handles content-length/chunked framing; this is not a streaming model API.

## Platforms and verification

**Native:** supported through the existing HTTP/TLS transport.
**Wasm:** remote-only, through the same fetch/Asyncify transport and
`tny_poll`. No sockets, subprocesses, or new platform conditional are added.
Node-wasm fixtures run in the wasm CI job. Browser use additionally requires
CORS permission at the endpoint and an environment credential supplied by
the host; there is no new browser login/settings UI. CORS failure is an error,
not a fallback to another model.

Offline checks are part of `make test`:

- `tnyjev_suite`: typed encoding, boundaries, malformed and duplicate fields,
  probability/choice validation, truncated prefixes, no-I/O preflight.
- `tests/integration/test_tnyjev.py`: real CLI and local HTTP, auth, stdin,
  structured state/criteria, model precedence, HTTP errors, response limits,
  one-byte chunks, every body split, timeout, and native interruption.
- Existing CLI/help alignment and shared HTTP split-boundary tests.
- `tests/mutation/mutate.py --focus tnyjev --fast`: decision-validation mutants.

No test requires a real TypeSafe key or paid inference. Live Jev behavior and
browser CORS must not be inferred from passing mock tests. See the
[local verification record](verification/tnyjev.md) for observed checks,
source identities, footprint and remaining evidence limits.

## Primary references

Consulted 2026-09-22:

- [HTTP API](https://docs.typesafe.ai/api)
- [Quick start and `TYPESAFE_API_KEY`](https://docs.typesafe.ai/introduction/quickstart)
- [Noul](https://docs.typesafe.ai/primitives/noul)
- [Choice](https://docs.typesafe.ai/primitives/choice)
- [Models](https://docs.typesafe.ai/models)
