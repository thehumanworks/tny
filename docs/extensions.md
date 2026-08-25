# Python extensions and event hooks

tny extensions are trusted Python programs that observe normalized agent
events and can request a small set of portable actions. Python support is
conditional on an external interpreter being available; tny does not bundle
one. The architectural decision is [ADR 0027](adr/0027-python-event-hooks.md).

> **Security warning:** extensions run with your full user permissions and are
> not sandboxed. They can read environment variables and credentials, change
> files, access the network, and run programs. Install only code you trust.

## Discovery

The only v1 entry-point forms are:

```text
~/.tny/extensions/reviewer.py
~/.tny/extensions/reviewer/index.py
```

`reviewer.py` and `reviewer/index.py` are the same extension name and therefore
collide; tny reports the collision and loads neither. Project-local extensions,
recursive scanning, and package entry points are not supported. This prevents
checking out a repository from executing its extension code automatically.

Extensions load by bytewise lexical extension name. One Python host remains
alive for the tny process and invokes listeners in extension order, then
registration order.

If no extension exists, tny does not start Python. If an extension exists but
the selected interpreter is unavailable, tny surfaces that fact and continues
without extension hooks.

## Typed entry point

An extension exports `setup(api)`. The supplied object provides typed event
dataclasses, action classes, constructors, and listener registration. The
module's annotations allow editors and type checkers to validate extensions
without tny shipping an interpreter.

Illustrative v1 shape:

```python
from tny_ext import AgentEndEvent, ExtensionAPI, continue_with, none


def setup(api: ExtensionAPI) -> None:
    @api.on(AgentEndEvent)
    def verify(event: AgentEndEvent):
        if event.stop == "done" and not tests_look_complete(event):
            return continue_with(
                "Check the remaining test failures before finishing.",
                message_kind="custom",
                custom_type="reviewer_follow_up",
            )
        return none()
```

This example selects a visible custom item attributed to `reviewer`. The
default `continue_with(...)` form instead selects a visible user follow-up.
Neither form inserts hidden system context.

The public Python names and normalized event schema are versioned at major 1.
The transport between tny and its Python host is private. Extensions must
ignore optional fields they do not use and should handle new enum values with
a fallback.

The installed host adds its own `lib/tny` directory to `sys.path`, so runtime
imports need no `pip install`. For editor/type-checker resolution, add that
same directory to the editor's Python extra paths (or add this checkout's
`python/` directory while developing tny itself). The package includes the
PEP 561 `py.typed` marker.

## Working examples

[`examples/extensions/`](../examples/extensions/README.md) contains tested,
copyable extensions for JSONL event logging, file-backed project context,
stopping after a failed tool, a one-shot verification continuation, and a
stateful directory package with relative imports. The examples are loaded and
invoked through the real Python host by `tests/extensions/test_examples.py`.

## Normalized event envelope

Every event has these common fields:

| Field | Meaning |
| --- | --- |
| `schema_version` | Normalized event major/minor version |
| `event_id` | Opaque unique event ID |
| `type` | Stable event discriminator |
| `sequence` | Monotonic order within the tny session |
| `provider` | Selected backend family |
| `session_id` | Stable tny session identity |
| `turn_id` | Provider/tny turn identity when applicable |
| `timestamp_ms` | Monotonic event time relative to process start |
| `payload` | Event-specific typed data |

The initial typed events are:

| Family | Events |
| --- | --- |
| Session | `session_start`, `session_end` |
| Agent request | `before_agent_start`, `agent_start`, `agent_end`, `agent_settled` |
| Stream | `text_delta`, `thinking`, `plan`, `status`, `steer_rejected` |
| Tools | `tool_start`, `tool_progress`, `tool_end` |
| Interaction | `permission_request` |
| Accounting/terminal | `usage`, `turn_end`, `error` |

Tool events carry the normalized tool name, correlation ID, bounded detail,
and completion result available from the backend. `agent_end` carries observed
assistant output, a normalized message view, and stop reason.
`before_agent_start` always exposes the outgoing prompt; system prompt, image,
and provider option fields are typed but may be empty where a host owns them.
Unknown event names become `UnknownEvent` and preserve their complete payload.

Provider-native payloads, when retained for diagnostics, are opaque and
optional. Portable extensions use the normalized fields.

## Candidate end and final settlement

One top-level user request can contain several provider turns:

```text
agent_start
  text/tool/reasoning events ... turn_end
  agent_end (candidate)
    continue? -> visible user follow-up or custom extension context
  text/tool/reasoning events ... turn_end
  agent_end (candidate)
agent_settled (exactly once)
```

`agent_end` is the only portable veto point after a provider turn ends. Its
payload includes the current extension iteration count and configured maximum.
`extensions.max_iterations` defaults to `0`, meaning unlimited. A positive
value caps extension-requested continuations for one top-level user request.
The cap is separate from the native tool/model step limit.

`agent_settled` is observational and fires exactly once. User cancellation,
an exhausted positive continuation cap, or the absence of a continuation
request settles the agent.

## Actions

Listeners return one of:

| Constructor/result kind | Effect |
| --- | --- |
| `none()` / `none` | No change |
| `context(content, custom_type, display)` / `context` | Add Pi-style custom context, visible by default |
| `continue_with(content, message_kind, custom_type, display)` / `continue` | At `agent_end`, add a visible user or custom item and start another provider turn |
| `stop(reason)` / `stop` | Request asynchronous cancellation; terminal events still drain |

All matching listeners run before actions are folded. Context items retain
listener order. `stop` wins over continuation. Multiple continuation
requests create one next provider turn containing each extension's visible
context in order.

`before_agent_start` accepts `context` or `stop`. Normal stream/tool/status
events accept `context` (queued to the next portable turn boundary) or
`stop`. Only `agent_end` accepts `continue`. Session lifecycle,
`agent_start`, and `agent_settled` are observational. An unsupported result is
reported and treated as `none`.

Exceptions, timeouts, malformed results, and host failures are reported with
the extension and event name, then fail open. They never interrupt the agent,
and a failing listener does not suppress later listeners. If timeout or host
death requires a replacement Python process, tny reloads the global
extensions, reports that process-local extension state was reset, and resumes
the remaining listener order. Returning an action on an event that does not
accept it is a visible diagnostic and otherwise acts like `none`.

Context cannot alter a provider request already on the wire. It is queued for
the next model/turn boundary, even where one provider offers a richer steering
operation.

## Configuration

Global `settings.json` may contain:

```json
{
  "extensions": {
    "enabled": true,
    "max_iterations": 0,
    "timeout_ms": 5000
  }
}
```

`max_iterations: 0` is unlimited. A positive value caps extension follow-up
turns for one top-level user request. `timeout_ms` applies to each handler.
The CLI overrides are `--max-extension-iterations N` and `--no-extensions`;
`TNY_EXTENSIONS=off` also disables loading. Repo `.tny.json` has no authority
over global extensions.

## Provider behavior

| Provider | Turn completion | Continue action | Stop confirmation | Important limit |
| --- | --- | --- | --- | --- |
| Native Responses/Chat | tny finishes its tool/model loop | another native model iteration with visible context | tny drains/synthesizes terminal | only backend where tny owns tools |
| Cursor SDK Bridge | `result`, then `done` | new `Send` on the same agent | cancelled `result`, then `done` | host owns tools; no portable pre-tool veto |
| Codex app-server | `turn/completed` | `turn/start` on the same thread | completed turn with `interrupted` | `turn/steer` is not portable and some turns reject it |
| ACP v1 | original `session/prompt` response | another prompt on the same session | response stop reason `cancelled` after final updates | client must keep draining after cancel |
| OpenRouter/generic compatible | terminal SSE/provider close | next HTTP model iteration | abort may not stop every upstream | mid-stream errors may still use HTTP 200 |

Provider terminal events remain true. `continue` creates a later turn; it
does not rewrite or reopen the completed provider turn. Session-end hooks are
tny lifecycle events because providers do not share a session-close event.

## libtny and wasm

libtny ABI 0 never discovers or executes `~/.tny/extensions`; a public
authority opt-in is deferred. CLI, TUI, and the ACP server use hooks through
the shared private runtime engine.

Python extensions are unavailable in wasm. Explicitly requesting them returns
an unsupported error; normal wasm operation does not start or emulate a Python
host.

## Verification matrix

| Lane | Required proof |
| --- | --- |
| Discovery | exact two entry forms, lexical order, duplicate-name collision, no project loading |
| Interpreter | zero spawn with no extensions; missing interpreter reports once and agent succeeds |
| Host lifecycle | one host across turns/sessions; clean shutdown; protocol/stdout isolation |
| Event schema | fixture for every v1 family, stable IDs/sequence, unknown optional fields tolerated |
| Ordering | extension order, listener order, context fold order, stop precedence |
| Failure isolation | exception, timeout, invalid action, broken pipe, and host exit all fail open; later listeners run |
| Continuation | visible custom attribution, explicit user follow-up, multiple messages, positive cap, `0` unlimited |
| Settlement | repeated candidate `agent_end`, exactly one final `agent_settled`, user cancel wins |
| Native provider | Responses and Chat text/reasoning/parallel tools/usage/error/terminal mappings |
| Cursor | SDK messages, deltas, tool lifecycle, result/done, cancelled and expired runs |
| Codex | item lifecycle, reasoning/tool deltas, usage, error-before-completed, interrupt |
| ACP | all v1 update variants, sparse tool updates, stop reasons, final updates after cancel |
| OpenRouter | comments, reasoning details, final usage, HTTP error and HTTP-200 mid-stream error |
| libtny | ABI 0 stays off, owner-thread behavior unchanged, no surprise stdout/stderr |
| wasm | default agent parity unchanged; explicit extension request fails cleanly |
| Security | isolated fixture demonstrates environment/file/process authority; warning present in help/docs |
| Regression | unit, integration, mutation, size, startup, and wasm suites remain green |
