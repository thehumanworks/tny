# 0027 — Python extensions consume versioned normalized events through one optional host

Date: 2026-08-24
Status: accepted

## Context

tny already maps Cursor SDK Bridge, Codex app-server, ACP, and the native
OpenAI-compatible loop onto a small internal event vocabulary. That vocabulary
is sufficient for the CLI and TUI to render text, tools, permissions, usage,
and terminal state, but it is not a safe extension contract: message and item
identity is lost, tool detail is clipped for display, provider stop reasons are
collapsed, and session lifetime is not represented.

[Pi's extension API](https://github.com/earendil-works/pi/tree/dcd461925db2edf69a43c8135db1180d418afd54)
demonstrates the desired product shape: user code registers event listeners,
may stop an agent, may add visible context, and may prevent a candidate agent
end so the agent performs another iteration. tny needs that control without
embedding or distributing Python, leaking provider wire schemas into user
extensions, adding a second agent loop, or weakening the wasm and libtny
contracts.

Provider capabilities are uneven. Codex can steer some active turns, Cursor
and ACP queue input while a turn is active, and the native loop can add context
only at its next model-call boundary. Cursor, Codex, and ACP host processes own
their tool loops, so tny cannot portably stop a tool before it executes. A
provider-neutral hook API must expose only control that tny can honor
consistently.

## Decision

### Trusted global Python extensions only

Version 1 discovers extensions only at these exact global paths:

```text
~/.tny/extensions/<name>.py
~/.tny/extensions/<name>/index.py
```

Entries are ordered by `<name>` using bytewise lexical order. If both forms
exist for one name, tny reports the collision and loads neither. It does not
scan project directories, recursive descendants, arbitrary files under the
extension root, Python entry points, site packages, or environment-specific
plugin directories.

Python is optional and external. tny does not package, link, download, or
install an interpreter. With no discovered extensions, no Python process is
started. With extensions present but no configured/supported interpreter, tny
surfaces one status diagnostic and runs the agent normally.

Extensions are trusted code with the user's full authority. They are not
sandboxed. They can read and modify every file available to tny, inspect its
environment and credentials, open the network, start processes, and retain or
exfiltrate event data. Installing an extension is equivalent to running a
local Python program. Restricting discovery to the user's global directory
prevents an untrusted repository from activating code merely by being opened;
it does not make installed extensions safe.

### One persistent host and one protocol owner

tny starts one Python host lazily per tny process and keeps it alive across
sessions and turns. The host loads all discovered modules, owns their Python
objects and registration order, and communicates with the C runtime over
private framed pipes. Extension stdout/stderr is captured as diagnostics and
cannot share the agent/provider protocol channel.

The host and tny begin with a version handshake. Every event and action
envelope carries a normalized schema version. Unknown optional fields are
ignored; an unsupported major version fails extension initialization, is
surfaced, and leaves the agent running without extensions. Provider-native
payloads may be included as explicitly opaque diagnostic data, but extensions
must not need them for portable behavior.

The pure-stdlib `tny_ext` module supplies typed dataclasses, action classes,
and registration annotations for editors and type checkers. An entry point
exports `setup(api)`, registers listeners on the supplied `ExtensionAPI`, and
returns without taking ownership of the loop. The concrete Python syntax is
specified in [extensions.md](../extensions.md); the wire protocol is private
and is not itself the user API.

### Extension lifecycle, not provider wire lifecycle

The normalized v1 surface adds `before_agent_start`, `agent_start`,
`agent_end`, `agent_settled`, `session_start`, and `session_end` around tny's
existing `text_delta`, `thinking`, `tool_start`, `tool_end`,
`tool_progress`,
`permission_request`, `plan`, `usage`, `turn_end`, `error`, `status`, and
`steer_rejected` events. Event payloads retain stable identity and ordering
fields where the provider supplies them. Provider events are mapped once
before they reach either the UI or extensions.

`agent_end` and `agent_settled` are deliberately distinct:

- `agent_end` is a **candidate end**. It fires after the provider turn has
  reached a terminal boundary but before tny declares the user's agent request
  settled. A listener may return `continue_with(...)`.
- `agent_settled` is the final, observational event. It fires exactly once
  after no extension requests another iteration, a user-configured extension
  continuation limit is reached, or the agent is stopped. It cannot be vetoed.

Naming a vetoable event `agent_settled` would make traces lie; naming only the
final event `agent_end` would leave no natural hook at which to continue.

### Pi-style custom context and explicit user follow-ups

The two context-bearing actions are intentionally different:

- `context(content, custom_type="tny_extension", display=True)` adds Pi-style
  custom context. It is attributed to the extension, retained in session
  context, and visible in the transcript by default.
- `continue_with(content, message_kind="user", display=True)` vetoes a
  candidate `agent_end` and starts another iteration. Its default is an
  explicit, visible user follow-up because every provider accepts that shape.
  The extension may instead select `message_kind="custom"` plus a
  `custom_type` when it needs a visible extension-context item.

Neither action mutates hidden system context. The TUI/JSON transcript records
the selected message kind and extension attribution, so a custom item is not
mistaken for text authored by the user.

Some provider wires accept only a user-input shape for the next turn. The
backend may lower the custom item into that provider shape, with an explicit
extension attribution wrapper, but tny's normalized transcript continues to
represent it as extension context. Extensions cannot request a hidden context
insertion in v1.

Context returned while a request is already in flight is queued for the next
portable model/turn boundary. tny does not promise immediate mid-stream
injection even when one provider has a steer operation.

### Action and listener ordering

For each event, tny and the host perform this sequence:

1. Freeze one versioned normalized event with a monotonically increasing
   session sequence number.
2. Invoke matching listeners in extension discovery order, then registration
   order within each extension.
3. Record every listener result. A thrown exception, invalid result, timeout,
   broken pipe, or host failure is surfaced with the extension and event name,
   then treated as `none`; later listeners still run. If a timeout or host
   death makes the process unusable, tny starts one replacement host, reloads
   the discovered extensions, reports that their process-local Python state
   was reset, and resumes with the remaining listeners for that event. A
   replacement-load failure is reported per remaining listener and still does
   not alter the agent.
4. Fold valid actions deterministically. Context additions retain listener
   order. `stop` wins over all continuation requests. Multiple `continue`
   results produce one next iteration with their messages in listener order.
5. Apply the folded action at the next safe runtime boundary, then publish the
   resulting lifecycle events.

Hook failures are fail-open by design: extension errors and timeouts never
interrupt, fail, or change the agent's stop reason. A faulty extension cannot
prevent later independent hooks from observing the event. Diagnostics are
visible rather than silently discarded.

The actionable v1 result kinds are `none`, `context`, `continue`, and `stop`,
constructed by `tny_ext.none()`, `context()`, `continue_with()`, and `stop()`.
An event documents which results it accepts. An action returned for an
observational-only event is diagnosed and ignored rather than being guessed
into a different action.

### Continuation policy

Extension-requested continuations are unlimited by default. The canonical
user setting is `extensions.max_iterations`; `0` means unlimited and a
positive value caps only continuations requested by extensions for one
top-level user request. `--max-extension-iterations N` overrides it for one
process; `unlimited` or `0` clears the cap. It does not change the native
model/tool step limit from [ADR 0024](0024-unlimited-steps-default.md) or a
host provider's own limits.

`extensions.timeout_ms` is the per-handler timeout and defaults to 5000 ms.
`extensions.enabled` defaults to true for CLI/TUI settings; `--no-extensions`
or `TNY_EXTENSIONS=off` disables discovery for the process. Repo `.tny.json`
cannot enable, disable, or configure global extension authority.

Every candidate `agent_end` includes `continuation_count` and the configured
limit. When a positive limit is exhausted, tny surfaces a status event, ignores
further `continue` actions for that request, and emits `agent_settled`.
User cancellation always takes precedence over extension continuation.

### Provider capability boundary

The v1 portable contract does not promise:

- a blocking `before_tool` hook for host-owned Cursor, Codex, or ACP tools;
- hidden system/developer-context mutation;
- injection into an already transmitted provider request;
- reopening a provider-native turn after its terminal event;
- synchronous cancellation confirmation.

`stop` requests provider cancellation and tny keeps draining until it
observes or synthesizes the terminal boundary. Cursor returns cancelled
`result` then `done`; Codex ends with `turn/completed: interrupted`; ACP may
send final updates before its original prompt response reports `cancelled`;
some OpenAI-compatible/OpenRouter upstreams may continue work after the HTTP
stream is aborted. The normalized trace distinguishes a stop request from the
confirmed terminal event.

A `continue` action starts another turn on the same provider session:
Cursor `Send`, Codex `turn/start`, ACP `session/prompt`, or another native-loop
model request. The provider's prior terminal remains true on the wire; tny's
agent-level lifecycle spans the extension-requested iterations.

### libtny and wasm

libtny ABI 0 does not discover or execute user extensions. Its explicit
runtime options intentionally have no extension-authority opt-in yet. A later
public opt-in must preserve owner-thread driving, expose the trusted-code
boundary, and add no callbacks from Python into arbitrary embedder threads.

The wasm build reports Python extensions as unavailable. It does not attempt a
browser Python runtime, remote extension host, or silent partial emulation. An
explicit extension request returns a clean unsupported error; ordinary wasm
agent operation remains unchanged.

## Verification gates

The implementation is accepted only with the matrix in
[extensions.md](../extensions.md), including:

- deterministic discovery, collision, and listener-order fixtures;
- exact event/action order for all four backend families;
- error, 5000-ms/default and configured timeout, invalid-result, host-exit,
  and later-listener fail-open tests;
- visible context and continuation-cap tests (`0` unlimited and positive cap);
- asynchronous stop confirmation and final `agent_settled` exactly once;
- interpreter-missing, libtny-default-off, and wasm-unavailable tests;
- a malicious fixture proving the documentation's full-user-authority warning
  is accurate without running it outside an isolated test directory;
- existing unit, integration, mutation, wasm, size, and startup gates.

## Consequences

- Users can write friendly, typed Python event listeners without tny carrying
  Python in its binary or starting it on extension-free runs.
- Provider differences are visible as capabilities and stop semantics instead
  of being papered over by a falsely universal pre-tool or hidden-context API.
- Extension control remains outside provider wire parsers and inside the one
  tny runtime loop, so CLI, TUI, ACP server, and opt-in libtny share ordering.
- A persistent interpreter increases attack surface and memory only for users
  who install extensions. Those extensions receive the user's authority and
  must be treated as executable software, not passive configuration.

## Local verification (2026-08-24)

Against baseline `aea57ad` on Apple Silicon macOS:

- stripped release size: 512,112 → 545,472 bytes (+33,360), below the
  1,887,436-byte Darwin budget;
- `tny --version`, 50 runs: 1.8 ± 0.4 ms baseline, 1.6 ± 0.0 ms with hooks;
- extension-free `tny status --json`, 30 runs: 1.6 ± 0.0 ms both builds;
- 200 ASan/UBSan C tests and 13 Python host/type/example tests passed;
- the focused runtime mutation pass killed every valid new lifecycle mutant;
  three survivors remain in unchanged allocation/poll-deadline paths;
- the complete fixture integration suite passed, including the installed
  support-package layout and native-provider extension continuation;
- wasm compilation was not run locally because `emcc` was unavailable; the
  source and CI contract retain the explicit unavailable path.
