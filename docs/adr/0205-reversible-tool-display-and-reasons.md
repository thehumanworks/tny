# 0205 — Reversible tool display and short reasons

Date: 2026-10-20
Status: accepted

## Goal

Make tool activity understandable at a glance without losing the details needed
to inspect or debug a call.

## Decision

Keep full normalized tool arguments and results separately from their rendered
cards. `ui.tool_calls_expanded` defaults to false. `/settings` changes this
persisted default immediately; explicit per-call choices survive a default
change. Fullscreen clicks toggle the paired argument/result cards in place.
The existing bounded transcript retains its scroll/resize behavior. Reflow
caches completed rows, so streaming text does not repeatedly wrap a previously
expanded large result. A resize or a card change rebuilds the affected view.

Inline output remains terminal-owned append-only scrollback. `ui.inline_mouse`
defaults to false, preserving ordinary terminal wheel scrolling and selection.
Opting in enables visible-heading clicks into a temporary full-detail inspector,
not an attempted rewrite of shell history. Mouse capture also captures wheel and
selection; Shift bypass is terminal-dependent and is disclosed in `/settings`.
Hit testing requires a fresh cursor-position report. `/tools [number]` provides
keyboard access even when a terminal cannot report positions
or a heading has scrolled away. Enter toggles details; Left/Right selects a call;
Up/Down/PageUp/PageDown scroll; Esc returns to the unchanged conversation and
draft. New backend output continues to be retained while inspecting.

The expanded default prints retained content in inline mode too. Display
expansion does not undo upstream tool output caps, alter model context, run a
tool again, or change session contents. Saved top-level tool messages can be
inspected on resume; nested events not present in a saved conversation are not
invented. Tool text is untrusted and cannot emit terminal controls.

`ui.tool_call_reasons` defaults to true. The system prompt requests a 1–10-word
purpose, not hidden reasoning. `run_code.reason` and nested-call reason metadata
remain optional at the execution boundary, so omitted explanations never fail
an otherwise valid call. Disabling the setting removes the prompt requirement.
Metadata is separate from nested tool arguments to avoid collisions with MCP
and plugin business fields. Private normalized events and runner transport keep
the reason; the public ABI is unchanged. Changing the setting rebinds an idle
runner or takes effect after the active runner turn, preserving its launch
snapshot. Explicit in-process and wasm execution read the changed setting at
the next provider request.

## Verification

Focused executable tests cover paired-card restoration, complete argument and
result tails, nesting, clicks after wrapping/resizing, unchanged streaming text,
explicit overrides, stale cursor reports, cached inspector rendering, settings
round trips, and optional reasons across code execution and runner events.
Existing TUI, protocol, ownership and SDK gates remain in place.
