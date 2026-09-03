# 0058 — Session control channel: roles and tool operations

Date: 2026-09-02
Status: accepted (implements ADR 0057's session-socket seam; amends 0053)

## Context

ADR 0057, the shell-first native-loop umbrella decision, makes human
interaction available to shell-first agents through `tny ask-user` and
`tny image attach`. Both verbs must reach the harness that owns the active
turn: reading `/dev/tty` would ask the wrong terminal after detach, fail in
background jobs, and bypass the TUI or editor frontend that owns the
conversation.

The session runner from [ADR 0053](0053-forked-turn-isolation.md) already has a
bidirectional NDJSON socket, but its original protocol has no client identity.
Every connection can send turn, cancellation, permission, and shutdown
operations. That is wrong once a subprocess can connect: an observer must not
answer a question, and a tool subprocess must not control the turn.

There is also a liveness problem. The native loop calls `terminal`
synchronously. While `terminal` waits for its child, the runner's outer poll
loop is not accepting or reading session clients. A child that connects and
waits for an answer therefore deadlocks with its parent. Re-entering backend
dispatch from the tool wait would be worse: the engine is already inside a
tool call and is not re-entrant.

Finally, an image attached during a tool call must use the existing pending
image path from [ADR 0008](0008-native-loop-images.md). Sending it as a tool
result would put image content under the wrong role, while waiting until after
the batch boundary would miss the next provider request.

## Decision

**The runner socket is a typed control channel with one owner, zero or more
observers, and short-lived tool clients. Every client declares its role before
it can use the socket, and every tool operation is correlated end to end.**

1. **Handshake and correlation.** The first client record is
   `{"op":"hello","role":"owner|observer|tool"}`. No other operation is
   accepted before it. The server hello/ack includes the role it assigned.
   There is at most one owner; attach clients use `observer`, and subprocess
   verbs use `tool`. An interactive owner adds
   `"can_answer_questions":true`; omission is the non-interactive owner used
   by `tny ask`, which takes the fallback path without waiting. Control request
   `id` values are non-empty, bounded JSON
   strings and are echoed unchanged in the matching event and result. An id is
   unique among that tool client's outstanding requests; stale, duplicate, or
   mismatched replies do not complete another request.
2. **Role allowlist.** Role is authorization, not display metadata.

   | Role | Client-to-runner operations |
   | --- | --- |
   | `owner` | `turn`, `steer`, `cancel`, `perm`, `end`, `detach`, `ask_user_reply` |
   | `observer` | `detach` only |
   | `tool` | `ask_user`, `image_attach` only |

   Observers receive the normal snapshot and event broadcast but cannot mutate
   the run or answer a question. Tool clients receive only handshake and their
   correlated control results; they do not receive the transcript. A
   disallowed operation is rejected and never changes runner state.
3. **Free-text questions.** A tool sends
   `{"op":"ask_user","id":"...","question":"..."}`. The runner forwards
   `{"ev":"ask_user","id":"...","question":"..."}` only to the owner.
   The owner answers with
   `{"op":"ask_user_reply","id":"...","answer":"..."}`; the answer is
   arbitrary text, not an allow/deny decision. The tool receives
   `{"ev":"control_result","id":"...","ok":true,"answer":"..."}` or a
   correlated `ok:false` result with an error. The TUI renders the question and
   sends the typed answer. The non-interactive `tny ask` owner returns the
   existing safe fallback text instead of prompting. If no owner exists, or the
   owner disconnects while a question is pending, the request fails closed:
   observers are never promoted, the question is not replayed to a later
   owner, and the waiting tool receives an error.
4. **Image attachment.** A tool sends
   `{"op":"image_attach","id":"...","path":"..."}`. The runner resolves
   the path under the same allowed workspace and extra-directory roots as file
   tools, rejects paths outside those roots, and detects PNG, JPEG, GIF, or WebP
   from magic bytes rather than the extension. Existing image size and count
   bounds still apply. The success result is sent only after the image has been
   added to the native loop's pending-image queue. The synchronous `terminal`
   tool cannot complete its batch before that enqueue is visible, so ADR 0008's
   batch boundary emits the image as user-role content on the next provider
   POST. Validation failure returns a correlated error and queues nothing.
5. **Bounded nested pump.** While `terminal` waits for a child, its poll loop
   may accept and service only tool control requests and the owning frontend's
   replies needed to finish those requests. It does not dispatch backend
   events, start another turn, or re-enter the engine. Normal backend dispatch
   resumes only after the terminal call returns. This is the same bounded-pump
   pattern used by the ACP server while one permission callback is outstanding,
   narrowed to the session-control descriptors.
6. **Child environment.** A local `terminal` child receives the runner's
   resolved socket path as `TNY_SESSION_SOCK` and its session id as
   `TNY_SESSION_ID`. The exported path is the actual listener path: for a deep
   session directory it is the ownership-checked `$TMPDIR/tny-<uid>/<id>.sock`
   fallback from ADR 0053, not the unusable `<session>/sock` candidate. These
   values describe the current runner and replace untrusted ambient values.
7. **No-socket and in-process modes.** `tny ask-user` and `tny image attach`
   are socket clients. With no `TNY_SESSION_SOCK` they print exactly one line,
   `tny: no session socket (set TNY_SESSION_SOCK or run inside tny)`, to stderr
   and exit 1 without reading a TTY. This is the clean wasm behavior too; wasm
   has no Unix session socket. `--ephemeral`,
   `TNY_ISOLATE=0`, and the macOS post-SecureTransport in-process containment
   from ADR 0053 likewise have no runner socket, so a subprocess CLI gets the
   same clean unsupported result. The macOS containment is distinct from the
   deep-path socket fallback: a pre-TLS forked runner exports its resolved
   socket normally, while an in-process turn has no listener to export.
8. **In-process frontend adapters.** `tny acp` server mode remains in-process
   and never creates a session socket. Its native `ask_user_question` path uses
   the ACP client's standard permission/question callback when that client and
   protocol surface can answer; otherwise it returns the existing
   non-interactive fallback. This does not claim universal ACP free-text input.
   The socket-bound `tny ask-user` and `tny image attach` subprocess verbs still
   fail cleanly in ACP server mode. libtny remains unchanged: an embedder may
   supply its existing frontend callbacks, but ABI 1 gains no socket or image
   entry point.

## Consequences

- A same-user socket connection no longer implies permission to control a
  turn. Attach remains observational, and a shell child gains only the two
  operations required by ADR 0057.
- Questions survive shell nesting without re-entering the backend, and owner
  loss has deterministic fail-closed behavior instead of hanging a child.
- Images attached by a child ride the next native provider request through the
  existing user-role pending-image mechanism. Host backends do not gain image
  injection from this channel.
- The protocol remains bounded NDJSON and retains ADR 0053's socket ownership,
  framing, and client-count limits. Split reads and writes are normal and must
  not change correlation or role enforcement.
- The environment variables are capability hints within the existing
  same-user session trust boundary, not portable credentials and not a remote
  transport. SSH tool execution does not synthesize a remote socket.
- wasm and every deliberate no-runner mode return a clean unsupported error;
  no fallback reads `/dev/tty`.
