# 0167 — Workspace sections and fuzzy dashboard navigation

Status: Accepted — 2026-09-22.
Amends the interactive dashboard presentation in ADR 0166.

## Problem

A global session inventory becomes difficult to navigate when unrelated workspaces
interleave by recency. Repeating the full path on every session consumes vertical
space without making workspace boundaries clear.

## Decision

Group the interactive saved-session dashboard by workspace directory. Place the
current context's cwd first, then other workspace paths alphabetically; list
sessions newest first within each section. Render each path as a heading above
indented, selectable session rows. Legacy rows in the current workspace storage
bucket use its known path for grouping and filtering; other unknown legacy
workspaces have an explicit heading. Preserve physical bucket plus session ID
as the selection identity.

Ordinary text input filters workspace paths using case-insensitive subsequence
matching. Matching a path keeps its sessions together. Backspace removes one
character; Esc clears a nonempty filter, or exits when it is empty. Ctrl-C/D exit
without stopping background work. The letter q participates in search; the
separate status-only `--run` view retains q to exit. Pasted text edits the filter
and cannot submit a session. This search does not start a provider or change
saved state.

Up/Down navigates sessions, skipping headings. Refresh preserves the selected
bucket and session when visible. Scrolling and resizing keep the selection and
its workspace heading visible within the available terminal rows. If only one
row is available, prioritize the selected session. Empty results show an explicit
message and Enter has no target. Opening/continuing uses the
existing owner checks and original workspace semantics.

Plain and JSON listings retain their existing global inventory and ordering.
This changes only TUI presentation and input, not discovery or native lifecycle.

## Verification

Unit tests cover grouping, fuzzy input, selection mapping and viewport budgets.
PTY regressions cover cwd priority, directory indentation, filtering and editing,
refresh identity, Unicode paste, short-terminal scrolling and session opening.
See [dashboard navigation verification](../verification/dashboard-navigation.md)
for the observed gates and independent review improvements.
