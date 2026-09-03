# Python extensions and event hooks

tny extensions are trusted Python programs that observe normalized agent
events and can request a small set of portable actions. Python support is
conditional on an external interpreter being available; tny does not bundle
one. The architectural decision is [ADR 0027](adr/0027-python-event-hooks.md).
The additive parity vocabulary, capability states, and provider authority
rules are frozen by [ADR 0028](adr/0028-extension-parity-contract.md) and the
[release-pinned manifest](features/extension-hook-parity.md).

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

`api.capabilities` is an immutable `CapabilityView`. It contains the provider
selected during setup plus complete native OpenAI, Cursor, Codex, and ACP
matrices. Capability entries have stable names, a current `supported`,
`unsupported`, or `unavailable` state, and a reason. Unknown names, unknown
future state strings, and optional fields are retained.

```python
def setup(api: ExtensionAPI) -> None:
    selected = api.capabilities.selected
    can_rewrite = selected is not None and selected.supports(
        "extensions.tool.pre.rewrite"
    )
```

The view is a setup snapshot. If the TUI later changes provider, use the
runtime event's `event.provider` with
`api.capabilities.for_provider(event.provider)`. Capability queries are static:
they do not spawn Python/providers, probe endpoints, read credentials, or
perform network I/O. `tny doctor --json` exposes the same matrix.

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

The public Python names and normalized event/action schemas remain at major 1;
capabilities begin at major 1. “Extensions v2” is a roadmap name, not a schema
major. The private host handshake negotiates all three majors. Legacy
protocol-1 initialization remains valid; an explicitly incompatible major is
reported and extensions fail open. Extensions must ignore optional fields they
do not use and handle new event, capability, and state values with a fallback.

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

The typed events are:

| Family | Events |
| --- | --- |
| Input/session | `user_prompt_submit`, `session_start`, `session_end` |
| Agent request | `before_agent_start`, `agent_start`, `agent_end`, `agent_settled`, `turn_start`, `turn_end` |
| Messages/stream | `message_start`, `message_update`, `message_end`, `text_delta`, `thinking`, `plan`, `status`, `steer_rejected` |
| Tools | `pre_tool_use`, `tool_start`, `tool_progress`, `tool_end`, `post_tool_use`, `post_tool_failure`, `post_tool_batch` |
| Interaction | `permission_request` |
| Configuration | `pre_compact`, `post_compact`, `compact_failed`, `model_change`, `effort_change`, `instructions_change`, `workspace_change` |
| Collaboration | `subagent_start`, `subagent_end` |
| Provider/accounting | `provider_request`, `provider_response`, `usage`, `error` |

Tool events carry the normalized tool name, correlation ID, bounded detail,
and completion result available from the backend. `agent_end` carries observed
assistant output, a normalized message view, and stop reason.
`before_agent_start` always exposes the outgoing prompt; system prompt, image,
and provider option fields are typed but may be empty where a host owns them.
Unknown event names become `UnknownEvent` and preserve their complete payload.

Raw provider payloads never cross the Python boundary. Native provider events
contain an allowlist only: fixed method/endpoint kind, status, wire kind, model
step, and stable logical-request/attempt correlation. Headers, URLs, bodies,
cookies, credentials, and provider error text are excluded.

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

ADR 0028 freezes these additive control actions. Native OpenAI implements every
listed control. Host adapters accept only the observation or real permission
surface reported by their capability matrix; returning an unavailable or
unsupported action produces a visible typed diagnostic and acts as `none`.

| Constructor / kind | Contracted boundary | Capability |
| --- | --- | --- |
| `transform_prompt(prompt)` / `prompt_transform` | replace submitted text before persistence/send | `extensions.prompt.transform` |
| `block_prompt(reason)` / `prompt_block` | reject submitted text before persistence/send | `extensions.prompt.block` |
| `rewrite_tool(arguments)` / `tool_rewrite` | replace native tool arguments before validation/permission | `extensions.tool.pre.rewrite` |
| `deny_tool(reason)` / `tool_deny` | prevent the exact native tool call from executing | `extensions.tool.pre.deny` |
| `decide_permission(decision, reason)` / `permission_decision` | allow-once, deny, or abstain on a real request | matching `extensions.permission.*` key |
| `annotate_tool(content, display)` / `tool_annotate` | add attributed post-tool metadata | `extensions.tool.post.annotate` |
| `replace_tool_result(content, is_error)` / `tool_result_replace` | select next-model result while retaining original | `extensions.tool.post.replace` |

Cursor reports its missing permission and host-tool control surfaces as
`unsupported`. Codex and ACP decisions remain unavailable until their adapter
lanes correlate the shared fold to a real live host request.

All matching listeners run before actions are folded. Context items retain
listener order. `stop` wins over continuation. Multiple continuation
requests create one next provider turn containing each extension's visible
context in order.

For the additive control plane, precedence is cancellation/`stop`, explicit
prompt/tool/permission deny, last valid transform/rewrite/replacement,
allow-once, ordered annotations/context, then abstain/none/failure. A folded
deny remains denied after a handler timeout or host restart and can never be
turned into an allow.

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

## Native tool transaction

Native calls run serially in provider batch order, including provider-declared
parallel calls. Each call has one stable provider/tool ID and this transaction:

```text
proposal
  -> pre_tool_use (observe, last rewrite, sticky deny, or stop)
  -> reparse + schema validation
  -> permission classification on the effective name and every target
  -> permission_request only if still unresolved
  -> tool_start -> execute -> tool_end
  -> post_tool_use | post_tool_failure
  -> persist effective result
post_tool_batch
  -> save the complete batch
  -> next provider request, or terminal
```

An extension `allow_once` is bound to that effective call only and never writes
a session grant or persistent rule. `abstain` leaves the ordinary frontend
decision path intact. A deny/stop never reaches built-in, shell, MCP, skill,
subagent, or future custom-tool execution. Rename/copy permission identity
includes both paths; selected MCP identity is `mcp:<server>/<tool>`.

Rewritten arguments are placed in provider history only after schema validation.
The top-level `extension_audit` ledger retains the immutable provider proposal,
effective arguments, original result, effective next-model result, action
attribution, error flags, and ordered annotations. It is never serialized as a
provider message. Replacements and annotations are bounded to 64 KiB. JSON CLI
tool summaries add original/effective status and a transformation marker only
when a transaction was transformed, preserving the prior shape otherwise.

Cancellation and extension stop use the same result/batch finalizer. Remaining
provider calls receive correlated interrupted results and post/batch observation
before the single interrupted terminal. CLI/TUI signal probes are checked after
every bounded Python control call and before execution or a later POST.

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
| Native Responses/Chat (openai, codex, claude, grok profiles) | tny finishes its tool/model loop | another native model iteration with visible context | tny drains/synthesizes terminal | only backend where tny owns tools |
| Cursor SDK Bridge | `result`, then `done` | new `Send` on the same agent | cancelled `result`, then `done` | host owns tools; no portable pre-tool veto |
| ACP v1 | original `session/prompt` response | another prompt on the same session | response stop reason `cancelled` after final updates | client must keep draining after cancel |
| OpenRouter/generic compatible | terminal SSE/provider close | next HTTP model iteration | abort may not stop every upstream | mid-stream errors may still use HTTP 200 |

Provider terminal events remain true. `continue` creates a later turn; it
does not rewrite or reopen the completed provider turn. Session-end hooks are
tny lifecycle events because providers do not share a session-close event.

For the complete 29-key current matrix, run `tny doctor --json` or see
[extension-hook-parity.md](features/extension-hook-parity.md). `unsupported`
means the selected provider cannot provide the semantic control;
`unavailable` means the frozen contract is not implemented or enabled in this
runtime. An observation capability can be supported while its rewrite/deny or
replace companions are unsupported, which is the precise meaning of
host-owned observational-only behavior.

After #55, native OpenAI supports every key except the two project-local keys,
which remain unavailable for #59. All providers share prompt/session/turn/
message/model/effort/instruction/workspace observation and control at tny-owned
boundaries. Provider-owned tool, permission, compaction, subagent, and redacted
wire cells remain unavailable or unsupported until their adapter lane proves a
real protocol surface.

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
| Native provider | prompt/tool rewrite and deny, permission allow/deny/abstain, post replacement/annotation, deterministic parallel batch, cancellation, recovery/resume, redacted request attempts, and Responses/Chat regressions |
| Cursor | SDK messages, deltas, tool lifecycle, result/done, cancelled and expired runs |
| Codex | item lifecycle, reasoning/tool deltas, usage, error-before-completed, interrupt |
| ACP | all v1 update variants, sparse tool updates, stop reasons, final updates after cancel |
| OpenRouter | comments, reasoning details, final usage, HTTP error and HTTP-200 mid-stream error |
| libtny | ABI 0 stays off, owner-thread behavior unchanged, no surprise stdout/stderr |
| wasm | default agent parity unchanged; explicit extension request fails cleanly |
| Security | isolated fixture demonstrates environment/file/process authority; warning present in help/docs |
| Regression | unit, integration, mutation, size, startup, and wasm suites remain green |
