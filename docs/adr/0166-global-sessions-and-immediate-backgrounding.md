# 0166 — Global sessions and immediate native backgrounding

Status: Accepted — 2026-09-22.
Amends ADR 0053 and supersedes ADR 0107's mandatory restart on Left and
workspace-limited dashboard. Checkpoint recovery remains available.

## Problem

After the caller initializes macOS SecureTransport, a fork-only child cannot
safely use Apple's trust runtime. Disabling isolation to avoid that crash makes
later turns depend on the TUI process and prevents backgrounding. Requiring a
tool-boundary restart also delays leaving a streaming or tool-running turn.
Filtering the dashboard by workspace and background origin hides otherwise
valid saved sessions when the user opens tny from another directory.

## Decision

Saved native CLI/TUI turns start a fresh executable session runner through the
existing host process seam. A private descriptor-only startup protocol transfers
the resolved context and runner options through bounded anonymous IPC. The
runner validates the inherited writer descriptor, retains any managed-worktree
usage lock, owns a detached process session and signals readiness before the caller submits work. Credentials are
never written to disk or argv. The caller retains startup responsibility until
the handshake succeeds; launch failure is an error, never an implicit switch
to fragile in-process execution. Provider startup remains lazy.

Backgrounding retains that running process. The runner persists the background
marker before acknowledging the request; the TUI then closes its owner
connection and opens the dashboard. No provider restart, tool completion,
continuation checkpoint or repeated prompt is needed to detach. HTTP and ACP
turns keep their in-memory state and current tool processes. The session writer
and listener stay owned throughout. Pending permissions retain their original
authority and wait for an owner rather than being silently approved. Questions
already queued during intentional detachment are replayed to the next owner,
with a five-minute bound. Explicit cancellation remains the existing bounded stop operation.

Left with an empty composer and no focused dialog opens the dashboard both
during a turn and while idle. Left still edits a draft or focused input.
Ctrl-X and `/agents` use the same dashboard. Repeated requests are harmless,
and leaving a reattached background view detaches again. Explicit ephemeral
sessions, `TNY_ISOLATE=0`, library-owned lifecycles and wasm retain their
documented process limitations; they cannot claim native detached persistence.

The dashboard scans all workspace buckets in the configured tny state directory,
without the ordinary session-list pagination cap. Saved foreground sessions
and completed, interrupted or failed sessions are included. Each row identifies
its workspace; text output sanitizes terminal control characters and JSON keeps
the exact workspace value and physical storage bucket. A legacy row missing its
workspace remains viewable, with an explicit current-cwd fallback notice before
continuation; its storage identity is preserved. Opening a row loads the session's saved workspace, provider and model; other
options follow existing continuation rules and current configuration. Live
reattachment retains the running turn's effective configuration. Listing and viewing do not require valid provider
credentials. A held writer lock and the runner owner handshake remain the
authority for attaching or continuing; discovery does not grant write access.

## Verification

[Acceptance and observed results](../verification/global-background.md) cover
caller TLS state, streaming/tool detachment, ACP, terminal loss, owner conflicts,
cross-workspace continuation and inventories exceeding 100 sessions. Local
fixtures use synthetic credentials. macOS results do not establish Linux,
Windows or browser runtime proof.
