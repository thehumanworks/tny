# Locked session reload — independent source review

**The helper and CLI slice are coherent; one caller gap remains before full A26 approval: TUI newly acquired runner startup must refresh persisted state under ownership.** This is static source review only. No runtime tests, interposers, injected hooks, fault programs or reproductions were executed. The separately frozen lifecycle approval in slice-review.md remains unchanged.

## Reviewed helper and CLI behavior

`session_reload_locked` requires an owned descriptor and resolves the already-selected session ID, never another `last` lookup. It loads/reconciles into a separate session object before replacing the original document/task body. Failure preserves the original document and descriptor. Success transfers only the durable working copy, preserving the original process-local extension identity and the same lock fd; closing the temporary object closes no owned descriptor.

`cmd_ask` retains its preflight task validation before any explicit steer. Its added reload runs after ordinary or post-steer ownership acquisition, before spawning/preparing a provider, so stale pre-acquisition transcript/task state is not reused. New sessions bypass the resume-only reload. The existing no-save resume refusal occurs earlier in session_open. Existing foreground parent close and background process-exit handoff remain unchanged.

The new unit case exercises refusal without ownership, a newer saved title/transcript/turn count, unchanged owned fd, competing-lock refusal, and a failed reload after document deletion that leaves ownership/state retained. The orphan-client isolation test now checks the actual flock before its one resume, retaining the terminal/result assertions and existing timeout; it does not introduce a fixed delay. These tests were inspected, not run here.

## Required TUI caller correction

The current TUI `runner_refresh_session` reloads on TURN_END. That event precedes old-runner engine shutdown and final save. Its BYE handler only drops the connection. A following `tui_runner_ensure` therefore can acquire the writer for a new runner while still holding the earlier in-memory document. An old owner's later final save may then be lost when the new runner saves its inherited snapshot. The initial TUI session argument and `/resume` likewise open/reconcile before the later runner acquisition.

Refresh persisted sessions after successful ownership acquisition and before listener binding/provider work. The shared spawn boundary can cover callers for which it newly acquired ownership. Distinguish truly new unsaved state explicitly; do not treat disappearance or corruption of a previously persisted session as permission to reuse/resurrect the stale document. Preserve process-local extension reason/sequence and any intended new-session task binding. If refreshing in the TUI parent, account for existing prewarm ctx-reader ownership before task reconciliation mutates ctx strings. Ordinary isolated prewarm uses the runner process itself, but thread-mode transitions still deserve deliberate handling.

This finding does not require changing active serve-turn behavior or reloading between turns inside the owning runner. Existing broad tests and final platform gates remain required. No full A26/runtime PASS is assigned.

## Inspected hashes

- `src/core/session.c`: `c526541ee5b73494a9809a4072b2581ab60687a18c9055da9f7883d758e5c658`
- `src/core/session.h`: `e792715b98f47e4fabb03ad386670c28535704b84445a2629403befe6a338de8`
- `src/cli/cmd_ask.c`: `479ecae3aba82e2d84671037633ca9432f39201bf0f09b54cd475aa812ce9ee5`
- `tests/test_core.c`: `c392f37675253839fca5a5aa8bc5f13e7a68208cc8333fbec4e235d3881447a3`
- `tests/integration/test_isolation.py`: `8eb5ebfde5a4c3883a2e7170620d1b7bd9119a0ac1fb9859742f8fd5455d5bfa`
- `src/tui/tui_runner.c`: `e646a5b4457d16dbd76bbce8f8c49f707de776459f3a97918edd725cf7d8151e`
- `src/tui/tui_commands.c`: `e76b2a45fdacce117e0a378448da947bf5b3886d53737943ea9098513d933399`
- `src/tui/tui.c`: `63750d616b55c6167458fda887a7f5bbbfcc05b292a3636e291a1d5ae1a35ea3`
- `src/tui/tui_prewarm.c`: `7218c117409137083dd378144e2fea6b686d4c0dbaaa46b81525a5e348ce3e95`
