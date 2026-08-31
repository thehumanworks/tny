# tnytty — HTTP API

The REST surface that makes sessions scriptable and shareable. Served by
`tnytty serve`, `tnytty run --listen`, or the durable broker configured by
`tnytty gui --listen`. The GUI listener addresses the broker's authoritative
sessions and remains available while they run after the GUI closes. JSON in,
JSON out (except the plain-text and binary screen formats). HTTP/1.1,
`Connection: close` per response in phase 1; SSE streaming is phase 3.

## Binding and auth (ADR 0002)

- Default bind: `127.0.0.1:7681`. Loopback binds require no token by
  default.
- Non-loopback binds (`--listen 0.0.0.0:7681`) **require** a token: use
  `--token T` / `TNYTTY_TOKEN`, or tnytty generates one and prints it
  once at startup. Requests then need `Authorization: Bearer <token>`.
- `--token` on a loopback bind enforces the token there too.
- Token comparison is constant-time. Failures are `401` with
  `{"error":"unauthorized"}`.

Sharing a session = sharing the base URL + token. Anyone with both can
read the screen and type into the pty — treat the token like an SSH key.

## Resources

### `GET /v1/health`

`200 {"ok":true,"version":"<ver>","sessions":N}` — no auth required on
loopback; token required elsewhere like everything else.

### `GET /v1/sessions`

`200 {"sessions":[{...session},...]}`

### `POST /v1/sessions`

Body (all optional):
`{"cmd":["bash","-l"],"cwd":"/workspace","cols":80,"rows":24}`.
Defaults: `$SHELL` (else `/bin/sh`), 80×24.
`201 {...session}` or `500 {"error":...}` if the spawn fails.
Sessions created through a `run` or `gui` listener are headless but use
the same event loop as the attached terminal or window panes.

### `GET /v1/sessions/{id}`

`200 {...session}`; `404 {"error":"no such session"}`.

The session object:

```json
{
  "id": "a1b2c3d4",
  "cmd": ["bash", "-l"],
  "cols": 80, "rows": 24,
  "title": "~/src — bash", "cwd": "/home/me/src",
  "alive": true, "exit_code": null,
  "created_unix": 1756400000,
  "graphics": 2, "generation": 42, "attached": true
}
```

### `GET /v1/sessions/{id}/screen`

- Default / `?format=text`: `200 text/plain; charset=utf-8` — the grid
  as UTF-8 lines, trailing blanks trimmed, one `\n` per row.
- `?format=json`: cursor, size, title, retained-history strings, and per-line
  text plus styled runs.
- `?format=wire`: `application/vnd.tnytty.snapshot-v1`, the canonical
  bounded, endian-stable VT snapshot used by native frontends.
- `?scrollback=N` prepends/includes at most the registry's configured
  retained history depth in the text/JSON formats.

JSON example:

```json
{
  "cols": 80, "rows": 24,
  "cursor": {"x": 3, "y": 0, "visible": true},
  "alt_screen": false,
  "lines": [
    {"text": "hi",
     "runs": [{"start":0,"len":2,"fg":"#ff0000","bg":"","attrs":["bold"]}]}
  ]
}
```

Colors: `""` default, `"@n"` for indexed n, `"#rrggbb"` for truecolor.
Attrs: `bold, faint, italic, underline, blink, reverse, hidden, strike`.

### `POST /v1/sessions/{id}/input`

Body `{"text":"ls -la\r"}` (UTF-8, written verbatim to the pty) or
`{"base64":"..."}` for binary-exact bytes. `200 {"written":N}`.
Control characters are the caller's job (`\r` for Enter, `` for
Ctrl-C); the API never rewrites input.

A pty master accepts only a kernel buffer's worth of input at a time.
Bytes that do not fit are **queued on the session** and drained by the
event loop as the child reads, so `"written":N` always equals the bytes
you sent and nothing is ever truncated mid-sequence — a half-delivered
`ESC[201~` would leave a literal `[201~` at the prompt.

The queue is bounded at **4 MiB per session** (`TT_INPUT_QUEUE_MAX`).
A write that would push it past the cap is rejected **whole** — nothing
is queued, ordering is preserved — with
`503 {"error":"input queue full"}`. That happens only when the child has
stopped reading; retry once it catches up. Request bodies are separately
capped at 1 MiB, so reaching the queue cap takes repeated posts.

### `POST /v1/sessions/{id}/resize`

Body `{"cols":120,"rows":40}` → grid reflow + `TIOCSWINSZ` + `SIGWINCH`
to the child. `200 {...session}`. A session attached to `run` or a GUI
pane gets its geometry from that frontend instead and returns `409`.

### `DELETE /v1/sessions/{id}`

`SIGHUP` to the child process group, reap, drop the session.
`200 {"ok":true}`. A session attached to `run` or a GUI pane gets its
lifetime from that frontend instead and returns `409`; close the
terminal or pane to end it. Teardown escalates to `SIGKILL` after
100 ms when a child ignores `SIGHUP`.

## Errors

Always JSON: `{"error":"<message>"}` with 400 (bad request/JSON), 401
(auth), 404 (unknown session or route), 405 (method), 409 (the attached
frontend owns session geometry or lifetime), 500 (spawn/OS), 503 (input
queue full — the child is not draining its input).

## Examples

```sh
tnytty serve --listen 0.0.0.0:7681 --token "$TNYTTY_TOKEN" &
curl -s -H "Authorization: Bearer $TNYTTY_TOKEN" \
     -d '{"cmd":["htop"],"cols":120,"rows":32}' \
     http://host:7681/v1/sessions
curl -s -H "Authorization: Bearer $TNYTTY_TOKEN" \
     http://host:7681/v1/sessions/a1b2c3d4/screen
curl -s -H "Authorization: Bearer $TNYTTY_TOKEN" \
     -d '{"text":"q"}' http://host:7681/v1/sessions/a1b2c3d4/input
```
