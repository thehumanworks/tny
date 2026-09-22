# 0166 — Slint is an optional desktop companion, not a second harness

Date: 2026-09-22
Status: experimental; Linux compile and fake-CLI checks only

## Context

A visually simple desktop chat can expose tny's sessions, usage, drafts,
images, SSH tools and durable swarms without duplicating the native agent loop.
The existing TUI remains a fast primary interface; the public libtny ABI is
experimental and deliberately has a different permission/MCP lifecycle from
the full CLI. Slint supplies a compact declarative Rust UI for the desktop
shell. iOS is a desirable future client, but neither the public ABI nor the
CLI-process surface currently supplies a tested iOS host.

## Decision

Keep `gui/` as a self-contained Rust/Slint desktop application. It invokes a
small, explicitly allowlisted set of the existing CLI's stable JSON verbs and
`ask --events=jsonl`; it does **not** implement providers, tools, mailbox
identity, permission policy, authentication or persistence. All CLI I/O runs
on workers; updates return through the Slint event loop. Input travels via
literal argument vectors or stdin, never interpolated into a shell. Bound
individual event/response sizes; suppress CLI stderr and do not display raw
tool arguments. A saved session must have its future local/SSH tool location
confirmed because historical SSH target details are not retained there.

Use explicit actions for operations with surprising effects: opening a parent
mailbox marks messages delivered, but never acks them; image generation can
commit an artifact even when manifest finalization fails; allowance lookup may
contact a provider. Passive mailbox status never consumes messages. Label
unknown token or publication state as unknown; neither worker prose nor
mailbox payload verifies work. Auto-completion does not advertise local paths
as remote paths.

`ask --events=jsonl` executes in-process in its CLI child and cannot make the
detached-runner crash-survival promise. The companion does not implement the
TUI's active-runner owner channel, permission reply, cancellation, full team
mutations, or checkpoint recovery. These are explicit limitations, not
emulated with unsafe CLI subprocesses. A future runner-owned GUI protocol
must preserve single-writer/ownership checks before this is called complete
parity. No new root Make target, C ABI layout, or provider implementation is
introduced. Build and test with `cargo` inside `gui/`.

## Platform contract

Linux desktop is the initial build/test target; macOS needs a native build and
manual UI smoke test. No iOS artifact or support claim is made. An iOS version
would require a separate compact mobile layout and a reviewed remote-host
protocol or a native iOS runtime port with permission/auth/file seams. Merely
sharing Slint declarations does not make child-process execution, terminal
ownership, microphone and local file access available on an iOS app.

## Verification

`cd gui && cargo test`, `cargo check`, `cargo fmt --check`, and a Linux
`cargo build` check the new adapter and compile the view. Fake-executable
tests cover literal argv/stdin, error redaction, response/event limits,
stream terminal conditions, metadata timeouts, resume provider selection,
image publication ambiguity and mailbox scope. The existing C `make test`
and `make quality` remain the core gates; neither substitutes for a visual
smoke or macOS/iOS execution. No live inference is part of this ADR.
