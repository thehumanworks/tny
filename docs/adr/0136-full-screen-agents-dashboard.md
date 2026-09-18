# ADR 0136: Full-screen agents dashboard

Date: 2026-09-18. Status: accepted.

## Context

The agents dashboard reused the transient menu block below the scrolling chat.
Opening it after a background handoff left the previous message on screen.
Starting `tny agents` also left shell output above the list. The dashboard looked
like part of the transcript rather than a separate view.

## Decision

On a successful interactive dashboard entry, clear the visible terminal and its
scrollback, then paint the dashboard from the top-left corner. Use the existing
ANSI home, erase-display and erase-scrollback sequences, not an alternate screen
or a new TUI framework. Terminals that do not implement erase-scrollback still
clear the visible screen.

Put the transition in `tui_agents_open`, shared by `tny agents`, `/agents`,
Ctrl-X and the acknowledged Left-arrow background handoff. Arming a handoff,
waiting for a tool boundary, or refusing an unsupported handoff must not clear
the chat. Refreshes and selection changes repaint the existing block without
repeated full-screen clears.

A small private drawing helper discards queued display text and the unfinished
streaming line, resets the old block's cursor coordinates, and queues the clear
for the next paint. This prevents pending chat text from appearing above the
list after the clear. It preserves composer drafts and persisted session data.
Reattachment continues to replay the saved transcript. Non-TTY text and JSON
output contain no new terminal controls and keep their existing format.

## Language and platform boundary

The affected module is ANSI painting and application view scheduling, not a
resource-ownership island. There is no new lifetime or manual resource cleanup
to improve with C++. Keep it in C11 under ADR 0114; retain the existing private
C++20 runner/session ownership modules and public C ABI. A language-only rewrite
would widen this UX fix without an ownership benefit.

The drawing helper uses the shared native/wasm renderer and adds no OS seam.
Wasm's existing inability to hand off an active in-process turn is unchanged.
This decision adds no backend or tool, and makes no performance claim.

## Verification

- Unit tests seed queued and partial transcript text plus stale block coordinates.
  The clear must replace those bytes with home/erase controls, reset coordinates,
  request a repaint and preserve the draft. Non-TTY calls must do nothing.
- Real PTY tests cover direct CLI entry, Ctrl-X and `/agents`, both normal and
  `--color=never` output, with old terminal text seeded before launch. The
  dashboard must start on row one, including after its timed refresh. Quit must
  restore cooked terminal mode. Plain and JSON CLI output remain escape-free.
- The existing real-runner fixture checks Left-arrow handoff after tool and
  no-tool turns, plus permission/steering paths. Its dashboard assertions use the
  terminal screen model rather than searching historical output. Existing
  reattach/follow-up checks continue to verify saved-session continuity.
- The new CLI screen regression fails against the pre-change binary: old shell
  text remains on row one. Run the unit, integration, quality and leak gates on
  the candidate, plus the focused dashboard mutation check.

See the [verification record](../verification/agents-dashboard.md) for commands,
results and review findings.
