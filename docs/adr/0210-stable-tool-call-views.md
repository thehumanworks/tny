# ADR 0210: Stable, reversible tool-call views

Status: accepted

## Goal

Let users choose visibility without letting tool payload size move the conversation.

## Decision

`ui.tool_call_view` selects compact (default), full, or hidden. Compact is one
physical row with status, a short purpose and live monotonic elapsed time;
fullscreen replaces that row, while inline appends one completion summary.
The existing activity indicator stays visible while inline tools run. Full
shows retained input/output, with the existing per-call toggle and inspector.
Hidden suppresses routine cards, never permissions, questions or failure notices.

The old expansion boolean is a fallback, not a second competing setting. New
mode wins when both exist. Writing the old boolean writes its equivalent mode.
No change to provider tool schemas, execution, prompt caching, or stored results.

Empty card anchors retain event order so changing fullscreen modes restores
cards in their original positions. Equal-position anchors have explicit sequence
numbers. Compact uses the transcript's Unicode cell widths. Replay never
manufactures timings. Timings subtract the engine producer timestamps, carried
through the private runner wire; batched socket delivery cannot become fake
zero-millisecond execution. Missing or reversed clocks omit the metric. Invisible details have an independent bounded tail because
hidden calls do not advance scrollback; active/viewed/newest records are exempt.
Visible records remain tied to scrollback retention so click targets stay valid.

## Evidence

Deterministic tests compare two overlapping calls: compact remains two rows from
start to completion, hidden is zero routine rows, and full exposes both complete
payloads. Repeated mode switches, narrow Unicode resize, pending-output eviction,
partial prose, replay and hidden-only retention have targeted regressions. Real
PTY tests cover all six shell/view combinations, inspector payload tails,
repeated toggles, resize, permission denial and terminal restoration.

This measures display stability, not provider latency or model token savings.
Upstream tool-output limits remain unchanged and recovery handles remain usable.
