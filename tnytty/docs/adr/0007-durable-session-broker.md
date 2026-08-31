# 0007 — A detached per-user broker owns GUI sessions; closing a frontend detaches, closing a pane kills

Date: 2026-08-30
Status: accepted

## Context

The original GUI owned its registry, pty masters and VT state in one process.
Every exit path therefore reached `tt_registry_free`, sent `SIGHUP`, and killed
the programs in every pane. Saving the split tree alone could not make a
session durable: a live pty master and its child process need an owner after
the AppKit process exits.

The lifecycle has two intentionally different user actions:

- Cmd-Q or the red traffic-light button means “leave my work running”; and
- Cmd-W on a pane or Cmd-Shift-W on a tab means “terminate these sessions”.

The REST API must still address the sessions the GUI displays. Moving the GUI
to a broker must not create a private registry beside a second public
`--listen` registry.

## Decision

### The broker is the process owner

Before initializing AppKit, `tnytty gui` connects to or starts one detached
broker for the current user. The broker owns the session registry, pty
masters, child reaping, authoritative VT state and pending input queues. It
continues pumping output when no GUI is running.

The local control endpoint is the existing HTTP surface over an `AF_UNIX`
socket. Its parent directory is owned by the user and mode 0700, the socket is
mode 0600, and accepted peers must have the broker's effective uid. Startup is
serialized with a same-user lock file. There is no second control protocol.

The GUI keeps a renderer-side VT mirror. It receives the canonical versioned
VT snapshot, applies it transactionally, and uses semantic generations to
coalesce repaint work. Input, resize, create, attach, detach and kill remain
HTTP operations on the broker registry.

### Frontend close detaches; pane and tab close kill

The GUI persists its tab/split/session-ID topology atomically. Cmd-Q, a red
window close, SIGTERM and ordinary GUI-loop shutdown save that topology and
detach every pane without destroying its broker session. A process-exit hook
applies the same policy when AppKit terminates from inside event dispatch.

Cmd-W kills the focused pane's broker session. Cmd-Shift-W kills every pane in
the focused tab. Closing the final pane or tab then closes the frontend, but
the explicit kill has already removed the affected session IDs.

On macOS, the window is not released when the red button is clicked. AppKit
may hide it before `tt_window_pump` observes the close; retaining it until
`tt_window_close` prevents the pump from dereferencing a released content
view and lets controlled detach complete.

### `gui --listen` belongs to the broker

The private Unix listener has a local-only administration route that asks the
broker to open the public TCP listener. That TCP listener uses the same
registry and the existing bearer-token rules from ADR 0002. Repeating the same
host, port and token is idempotent. A conflicting configuration returns an
error instead of disrupting another frontend's listener. The public listener
stays up with the durable sessions after the GUI exits.

## Consequences

- Closing and reopening the GUI reattaches the same session IDs and child
  PIDs. Output produced while detached is present in the authoritative VT.
- The durability boundary is the broker process. A broker crash, logout or
  machine reboot cannot preserve local child processes; stale saved topology
  is discarded rather than presented as live.
- Public callers may read and write attached sessions but still receive 409
  for resize or delete. The same-uid GUI path may perform its explicit resize
  and kill actions.
- The broker and GUI each keep one event loop in separate processes. No
  thread shares or mutates a VT instance across the process boundary.
- The public listener can outlive the command that requested it. Changing an
  active listener requires stopping the broker, so a second GUI cannot
  silently rotate another frontend's token.

