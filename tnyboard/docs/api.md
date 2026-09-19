# tnyboard API v1

## Transport and envelopes

```python
from tnyboard import handle, render

response = handle("/physical/project/path", "demo", {"op": "get"})
if response["ok"]:
    print(render(response["result"], width=100))
```

`handle(root, board, request)` returns a JSON-compatible envelope; it does not
raise on ordinary request/storage errors. It does not mutate the request object.
`render(snapshot, width=100)` returns a string without a trailing newline. It
accepts a successful get/list snapshot (or its success envelope). Its input must
be a validated snapshot, not arbitrary untrusted structures. Width clamps to
1..1000. Output contains printable ASCII and line breaks only.

```
Success: {"ok":true,"result":VALUE}
Failure: {"ok":false,"error":{"code":"revision_conflict","message":"Revision conflict"}}
```

`python3 -m tnyboard --root ROOT --board NAME rpc` reads **one** UTF-8 JSON value
through EOF (at most 1 MiB), writes **one** compact JSON envelope plus newline to
stdout, and exits 0 on success / 1 on operation or JSON error. CLI argument errors
use argparse stderr and exit 2, before the RPC transport starts. A second JSON
value, trailing non-whitespace, duplicate keys at any nesting depth, non-finite
numbers and invalid UTF-8 are rejected. The Python dict API naturally cannot
represent duplicate keys. Unknown request fields are errors; no aliases.

### HTTP

Set `TNYBOARD_TOKEN` to a securely generated value, then run:

```sh
python3 -m tnyboard --root "$PWD" --board demo serve --port 8765
```

The environment variable must contain 16..256 printable ASCII non-space
characters. The server binds **127.0.0.1 only**, has no host flag, writes the bound
address to stderr, and stores/logs no token. `--port 0` selects a dynamic port.
`tnyboard.http.make_server(root, board, token, port=0)` returns the same server for
embedding/tests; caller owns `serve_forever()` and `server_close()`.

- `POST /v1/rpc`: same JSON request/envelope as CLI/Python.
- `GET /v1/health`: `{"ok":true,"result":{"schema_version":1}}`; no storage read.
- Both require exactly one `Authorization: Bearer TOKEN` header. Use HTTPS
  termination only within an independently secured deployment; no remote access
  feature is supplied here.
- POST requires exactly one `Content-Length` (decimal, <=1048576) and exactly
  one `Content-Type: application/json`. No chunked transfer encoding.
- No CORS headers. Requests with `Origin` are rejected. Other methods/paths fail.
- A five-second total request-read deadline bounds headers and body, including
  slow-drip requests. Incomplete bodies, malformed framing and duplicate JSON
  keys fail. An incomplete request line/header may close without a JSON reply;
  parsed malformed requests receive structured errors when the socket permits.
- The server handles one connection at a time, closes after one request, and
  does not persist credentials. This bounds admission but a slow request can
  delay others until its deadline. Storage transactions may wait for the board
  lock; the network deadline does not cancel a mutation already admitted.
- HTTP 200 success; 400 invalid input/path; 401 bad bearer; 403 owner/origin;
  404 absent board/ticket/endpoint; 409 revision/claim/dispatch/already-exists;
  413 size; 415 content type; 408 body timeout; 500 corrupt state/storage.
  Unsupported methods/HTTP syntax use structured `http_error` where possible.

A timeout/lost acknowledgment is **not proof of failure**. Read the stored
revision/intent before deciding what happened. Never blindly repeat a launch.

## Requests

Every request includes `op`. Every mutation includes a valid `actor`. Every
mutation except `init`/`create` includes integer `expected_revision >= 1`; for
`configure` this is the **board** revision, otherwise the **ticket** revision.
All ticket mutations include `id`. `token` is optional syntactically for every
ticket mutation; it is mandatory semantically for claimed worker changes except
comments. `token` is a nonblank string, <=256 UTF-8 bytes.

| op | Additional required fields | Optional fields | result |
|---|---|---|---|
| `init` | — | `columns`, `lead` | snapshot |
| `get` | — | `id` | ticket with id; otherwise snapshot |
| `list` | — | — | snapshot (all tickets, sorted by id) |
| `create` | `title` | `id`, `status`, `body`, `owner` | ticket revision 1 |
| `configure` | — | `columns`, `lead` (at least one) | board |
| `move` | `status`, `reason` | `token` | ticket |
| `comment` | `body` | `token` (ignored) | ticket |
| `claim` | — | `token` | ticket with claim |
| `release` | — | `token` | ticket with null claim |
| `assign` | `owner` (nullable) | `token` | ticket with null claim |
| `dispatch-intent` | `intent_id` | `token` | ticket with pending dispatch |
| `dispatch-result` | `intent_id`, `outcome` | `job_id`, `detail`, `token` | ticket with updated dispatch |

Get/list reject actor and expected_revision: they are read-only. No mutation is
implicitly idempotent. A successful ticket mutation increments its revision once
and adds exactly one history event, even movement to the same column. Configure
increments the board revision even if the supplied configuration is unchanged.
Init on an existing board and create with an existing ID fail without overwriting.

### Values/defaults

- Board names, ticket IDs, column IDs, intent IDs: ASCII
  `[A-Za-z0-9][A-Za-z0-9_-]{0,63}`. IDs are case-sensitive; use consistent lower
  case on case-insensitive filesystems. Generated ticket IDs: `t-` + 16 hex digits.
- Actor/owner/lead labels: `[A-Za-z0-9][A-Za-z0-9_.@-]{0,127}`. Actor never null.
  Owners/lead may be null. Lead defaults to `board-lead`; actor `user` always has
  explicit local-operator oversight. Init must be performed by its lead or user.
- `columns`: 1..32 unique `{id,title,owner}` objects, all three fields required.
  Title is nonblank, <=256 UTF-8 bytes. Default IDs `todo`, `doing`, `done`;
  titles `To do`, `Doing`, `Done`; each owner `worker`.
- Create: nonblank title <=1024 UTF-8 bytes; body defaults to `""`, <=65536
  bytes; status defaults to first column ID; owner defaults to null. Any valid
  actor can create a ticket and set its initial override.
- Move: status must exist; reason is nonblank and <=8192 UTF-8 bytes.
- Comment: body is nonblank, <=65536 UTF-8 bytes. Actor may differ from owner
  or claimant. Comments never clear claims or change dispatch state.
- Claim: token supplied by caller or generated using `secrets.token_urlsafe(32)`.
  Claiming an already claimed ticket requires the old token for workers; lead/user
  may replace it. Lead/user dispatch intervention also clears another actor's
  claim. There is no separate renewal or expiry. Release requires a claim.
- Assign: nullable owner override; null restores column inheritance.
- Limits: each serialized request/stored document <=1048576 bytes; <=10000
  tickets per board. History is append-only and shares the document limit;
  exceeding it rejects the whole mutation without truncation. Snapshots may be
  larger than one document; there is no pagination in v1.

## Persisted types

`schemas/{request,result,board,ticket}.schema.json` are JSON Schema Draft 2020-12.
Schemas define structural shapes; runtime additionally enforces UTF-8 byte limits,
paths, unique column IDs, existing status references, exact history/revision
relations, owner/claim checks, and dispatch transitions. JSON Schema `maxLength`
is a character bound, not a replacement for the runtime UTF-8 byte bound.

Board: `{schema_version:1,name,revision,columns,lead}`.
Snapshot: `{board:BOARD,tickets:[TICKET,...]}`.
Ticket:

```json
{
  "schema_version": 1,
  "id": "parse",
  "revision": 1,
  "status": "todo",
  "title": "Implement parser",
  "body": "",
  "owner": null,
  "comments": [],
  "history": [{"op":"create","actor":"user","at":"2026-01-01T00:00:00+00:00","revision":1,"detail":{}}],
  "claim": null,
  "dispatch": null
}
```

Comments: `{actor,body,at,revision}`. History:
`{op,actor,at,revision,detail}`. Timestamps are UTC ISO-8601 strings produced at
mutation time, informational only (never ordering/fencing authority). History
revisions run from 1 through current revision with exactly one initial create.
Details: empty object for create/comment/claim/release; `{from,to,reason}` for
move; `{from,to}` nullable owner labels for assign; `{intent_id}` for dispatch
intent; full dispatch object for dispatch result. Claim tokens are not repeated
in history. Claim: `{actor,token}` or null. History preserves old column IDs even
if an unoccupied column is removed later.

Dispatch is null until the first intent, then:
`{intent_id,state,actor,owner,created_at,updated_at,job_id,detail}`. Actor is the
reserving actor; owner is effective owner **at reservation**, nullable. State is
`pending|confirmed|failed|uncertain`; job_id nullable (nonblank <=256 bytes if set);
detail is a string <=8192 bytes. Confirmed requires job_id. Previous outcomes
remain in history. Job identity is an opaque durable launcher identity, not proof
of execution completion or acceptance.

## Adapter dispatch protocol

1. Select the explicit physical project root and board. Fetch the ticket, resolve
   the effective owner to the adapter's existing task definition. No implicit
   provider calls on get/list/show/TUI refresh. Lead dispatch is a separate explicit
   adapter action; tnyboard supplies no scheduler or launcher.
2. Generate a fresh intent ID. Submit `dispatch-intent` with current revision,
   actor and any needed claim token. The component reserves it **under the board
   lock and durably writes it before returning**. Intent IDs cannot be reused
   anywhere in the board, including after failure. Pending/uncertain state prevents
   another intent on the same ticket. Only launch after receiving this success.
3. Launch once, bound to that same root/board and the reserved ticket/intent. Do
   not hold the storage lock while launching. Do not equate reservation to launch.
4. Submit `dispatch-result` with the reservation's returned revision and exact
   intent ID. `confirmed` requires returned durable `job_id`; `failed` means the
   launcher is known not to have launched; `uncertain` means the launch/ack could
   have happened. Include diagnostic `detail` without secrets. If another ticket
   mutation changed the revision, read and reconcile before submitting the result;
   do **not** run the launcher again. Changed ownership may require lead/user.
5. Only pending/uncertain outcomes can be resolved. Uncertain remains blocked
   until explicit reconciliation records confirmed or failed. Confirmed/failed
   cannot be rewritten; a later, explicitly requested launch uses a new ID.
   Moves/assignment clear claims but do not clear dispatch uncertainty.

No exactly-once guarantee exists across an external launcher and filesystem.
A crash after reservation may leave a pending intent without a launch; a lost
ack may leave a launched job pending. Both require manual reconciliation, never
an automatic retry. Multiple tickets can have independent pending intents.
Confirmed prior launches do not prevent a new **explicit** dispatch; the adapter
must decide whether overlapping jobs are appropriate.

Example CLI request (actor `user` is the trusted local operator):

```json
{"op":"dispatch-intent","id":"parse","actor":"user","expected_revision":3,"intent_id":"run-001"}
```

After successful reservation (ticket revision 4) and a confirmed launcher result:

```json
{"op":"dispatch-result","id":"parse","actor":"user","expected_revision":4,"intent_id":"run-001","outcome":"confirmed","job_id":"durable-job-id"}
```

Error codes: `invalid_request`, `too_large`, `not_found`, `already_exists`,
`revision_conflict`, `forbidden`, `claim_conflict`, `dispatch_conflict`,
`unsafe_path`, `corrupt_state`, `storage_error`; HTTP also `unauthorized`,
`http_error`, `timeout`. Messages are diagnostic, not a stable parsing interface.
Failed validation/authorization/revision checks leave persisted JSON unchanged.
I/O errors or lost transport acknowledgments require read-back reconciliation.
