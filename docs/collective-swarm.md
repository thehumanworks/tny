# Collective swarm mode

Use `tny --swarm ask "review this design"`, `tny ask --swarm=3 "implement and verify"`,
or `/swarm 3` in an idle interactive conversation. The current session facilitates
collaboration: peers can challenge each other, exchange evidence and converge on a
verified result. Mode does not force delegation or discussion on trivial tasks.
Your `--task` preset and system instructions remain in effect.

The optional positive count is a maximum of 1..16 collaborators, excluding you/the
initiating lead. No count means the lead chooses within runtime limits. Both global
and ask-local flags accept `--swarm 3` and `--swarm=3`. A following nonnumeric prompt
is not consumed. Numeric-looking malformed counts fail; use `--` before a numeric
prompt. `/swarm` enables without choosing a count. Mode and cap survive resume,
runner rebind and checkpoints. Use `/new` before changing an established cap.
Enabling waits for the idle writer to release its session, preserving its transcript.
Pre-existing owned live jobs or uncertain cleanup must finish before adoption.

The lead starts worker-only teams through `team_control`/`tny team` or job tools.
All run launches share the parent session's admission cap, including repeated starts.
Set job `concurrency` high enough for peers that must discuss simultaneously; the
existing default is 2. Do not occupy every runnable slot waiting on queued peers.
Nested collaborators and isolated subagents cannot create extra collaborators.
Legacy job retry is refused in mode; create new parent-owned tasks instead. Existing
DAG dependencies, roles, workspaces and attempt outcomes remain authoritative.
The cap protects harness launch paths; it does not sandbox arbitrary same-user code.

## Peer messages and shared channel

Each durable run is a channel. Private `team_mailbox send` remains available.
`publish` atomically addresses active peers and the lead, excluding the sender:

```sh
tny mailbox publish --run RUN --id design-v1 --text '{"topic":"design","thread":"a","type":"proposal","body":"evidence"}'
tny mailbox wait --run RUN --timeout-ms 30000
tny mailbox ack --run RUN --id RECEIPT_ID
```

Typed tools use the same fields: action, run, id/text for publish, timeout_ms for
wait. Use member/thread-prefixed IDs to prevent competing proposals from colliding.
Publication IDs use 1..48 ASCII letters/digits/dot/underscore/hyphen. Each
recipient receives its own receipt ID; acknowledge the returned ID, not a computed
one. Retry with exactly the same ID/body to reconcile the original set even after
peers finish. Changed content/sender or collisions fail. Publication returns compact
receipt metadata, not N repeated payloads. Peers read payloads through wait/inbox or
native automatic safe-boundary delivery. No new permission is granted by a message.

Envelope fields are optional conventions: `topic`, `thread`, `type`, `body`, with
proposal/challenge/reply/evidence/decision types. They do not add routing or state.
Payloads are UTF-8, at most 16KiB. At most 64 receipts may be outstanding per
recipient and 256 retained per run. A 16-recipient publication consumes 16 history
rows. Acknowledgement frees outstanding space, not retained history. A full recipient
or history rejects the whole publication; there is no partial success or eviction.

Wait is native and event-driven, with no model polling or periodic mailbox scans.
`timeout_ms` is required, 0..30000. Zero gives `MAILBOX_EMPTY` when nothing is queued;
a positive deadline gives `MAILBOX_DEADLINE`; terminal state gives `MAILBOX_TERMINAL`
after pending receipts are returned. These observational outcomes exit 0 with
`ok:false` and the named outcome in `error`. Cancellation returns 130 with
`MAILBOX_CANCELLED`; stale/denied/corrupt/watch-error outcomes fail. Send/publish
refusal always exits nonzero. Transient lock contention retries for at most 250ms.

Messages remain replayable until explicit ack. Native delivery persists context
and its dedup ID before marking delivered. It does not interrupt/replay busy tools,
implicitly acknowledge, promise exactly-once effects or guarantee convergence.
The stable bounded policy is a cache-friendly prefix; actual cache hits depend on
the provider and are not established by localhost fixtures.

## Support

| Context | Collective mode / wait |
| --- | --- |
| Native local saved Darwin | kqueue; tested with localhost fixtures |
| Native local saved Linux | inotify implementation; platform execution evidence tracked separately |
| wasm, Windows/MSYS | explicit unsupported before mode effects |
| SSH, ephemeral, embedded | explicit unsupported before mode effects |

See [team control](team-control.md), [mailboxes](team-mailbox.md),
[ADR 0156](adr/0156-collective-swarm-mode.md) and
[verification evidence](verification/collective-swarm/evidence.md).
