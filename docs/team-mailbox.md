# Durable team mailboxes

The public `mailbox` CLI, native `team_mailbox` tool and terminal-profile adapter
share the durable mailbox service. Native delivery is active: it adds untrusted
collaboration context at the next model-call boundary, without interrupting or
replaying a running tool. This is not a second execution queue or provider loop.
Existing synchronous `subagent.message` is a separate surface.

## Parent identity and indexed members

A captured top-level parent is recorded as `parent_session_id` in the team job.
Its mailbox task is **-1**, spelled `lead` by the CLI. Indexed items are 0-based;
a `role:"lead"` item is still an indexed member, not mailbox -1 and not the parent.
The runtime binds native callers to the saved session or private run/task/attempt
capability. JSON sender fields and user-supplied session IDs cannot grant authority.
Do not copy private capability environment values into prompts, logs or commands.

Parent-to-worker and worker-to-parent messages do not need peer opt-in.
Messages between indexed members require `peer_messages:true` on the job.
Roles alone do not enable peer messages. Attempts fence both endpoints. A retry
does not inherit old messages as current work. Terminal/cancelled endpoints
reject new sends; existing current-attempt delivery/ack can finish. Repeating
an exact prior send retrieves its receipt rather than enqueueing another message.

## Public CLI and native tool

Replace `RUN_ID` with the returned job ID and `0` with the intended task index:

```sh
tny mailbox send --run RUN_ID --to 0 --id clarification-1 --text 'Review only the parser'
tny mailbox send --run RUN_ID --to lead --id question-1 --text 'Which parser version?'
tny mailbox inbox --run RUN_ID
tny mailbox read --run RUN_ID --id question-1
tny mailbox ack --run RUN_ID --id question-1
tny mailbox retire --run RUN_ID --to 0 --before-attempt 2
```

These examples represent different authorized callers: a worker sends to `lead`;
the parent/operator reads the parent's queue. A caller reads/acks its own queue,
not an arbitrary recipient's. `--json` is accepted; output is always JSON.
`retire` is parent/operator-only and applies only to strictly older job attempts.
It is not a way to drop current work.

The all-tools profile uses, for example:

```json
{"action":"send","run":"RUN_ID","to":-1,"id":"question-1","text":"Which parser version?"}
```

Pass this to `team_mailbox`. `to` is an integer: -1 for parent or a task index.
For `inbox` supply only `action` and `run`; for `read`/`ack` also supply `id`;
for `retire` supply `to` and `before_attempt`. Terminal-profile agents use direct
`tny mailbox ...` commands so the adapter retains the captured caller context.
Do not wrap them in a pipeline or nested shell and assume identity transfers.

## Send, delivery and acknowledgment

`send` persists before returning a receipt. It neither stops a busy terminal tool
nor restarts the worker's turn. Message content is explicitly marked untrusted.
It cannot authorize execution, verification, acceptance or integration, or change
system instructions. The recipient must use its own task and permission scope.

Native automatic delivery saves the context and receipt in the session, marks
it delivered, and advances a persisted cursor. A message does not reappear at
each later model call. This does **not** ack it. Explicit CLI/tool `inbox` and
`read` also mark returned messages delivered without acking them. Both delivery
paths leave the message available in the inbox until a separate `ack`; loss of
stdout can therefore be recovered. Ack after processing to release outstanding
queue capacity. An ack of a queued, never-delivered message refuses. Repeated ack
is idempotent. Underlying C read/inbox snapshots alone do not mark delivery; the
public adapter performs that step.

On uncertain send/I/O or client loss, retry the **same ID and content**. Exact
duplicates return the original receipt. Reusing an ID for different content or
endpoints returns `MAILBOX_CONFLICT`. Do not turn a timeout into a new logical
message ID. No exactly-once external side effects are promised.

## Retired queues and limits

Storage is `<job-dir>/mailbox.json`, under the job's state lock. Entries retain
sequence, job attempt, sender and recipient task/attempt, payload and state:
queued (0), delivered (1), acked (2), retired (3). Retry does not retarget them.
Older unacked messages can continue to occupy outstanding capacity. Explicit
retirement converts older queued/delivered records to retained tombstones;
it does not delete history, rewrite current messages or imply the worker read them.
A repeated retirement can report zero because the earlier call already applied.

| Bound | Behavior |
| --- | --- |
| Payload | At most 16,384 UTF-8 bytes; no embedded NUL. |
| Message ID | 1–64 ASCII letters/digits/`.`/`_`/`-`, unique across the run. |
| Outstanding | At most 64 queued/delivered messages per recipient across attempts. |
| History | Bounded; exhaustion refuses rather than silently deleting evidence. |

Busy, denied, stale, terminal, full, history-full, corrupt and I/O errors are not
acceptance. Retry transient lock contention with a bound. Invalid/corrupt records
fail closed. Atomic replacement and file/parent-directory sync precede successful
mutation responses; an I/O error after publication is not proof of rollback.
Physical power-loss survival and exactly-once effects are not inferred from this.
Retired state is retained so an old message cannot replay into a later attempt.

## Support boundary

Supported: saved native local parents and workers using the native provider loop,
in both all-tools and terminal profiles. Unsupported: wasm/SSH/embedded mutation
and host-backend automatic context injection. The private identity and worktree
rules are not an OS sandbox against other programs with the same user's privileges.
See [team control](team-control.md) and
[ADR 0139](adr/0139-durable-team-mailbox.md).
