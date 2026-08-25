# 0028 — Extension parity is capability-scoped and versioned

Date: 2026-08-25
Status: accepted

## Context

[ADR 0027](0027-python-event-hooks.md) established trusted global Python
extensions, normalized v1 events, visible context, continuation, cancellation,
and final settlement. Pi v0.84.3, Codex rust-v0.149.1, Claude Code v2.1.245,
and fx v0.0.5 expose additional lifecycle and control points. Their product
operations and authority boundaries differ, so copying names or comparing raw
event counts would claim controls that tny cannot safely provide.

In particular, tny owns prompt submission and the native OpenAI-compatible
tool loop. Cursor, Codex, and ACP hosts own their tool execution. A host-owned
tool may be observable without being blockable or rewritable. Project-local
Python also creates an authority boundary that cannot be enabled merely by
checking out a repository.

Parallel provider work needs one vocabulary, ordering rule, capability model,
and compatibility policy before any adapter invents local semantics.

## Decision

### Parity definition

tny targets **capability-scoped hook parity**:

- Where tny owns the boundary, a contracted action is implemented with the
  same material effect and ordering as the compared hook.
- Where a host owns the boundary, tny exposes reliable observations and real
  protocol decision surfaces only. Unsupported control is never simulated.
- Where tny lacks the underlying product operation, the comparison is marked
  `operation_absent`; no synthetic event is emitted merely to increase a count.
- A safety exclusion is explicit and receives `unsupported_safety` in the
  pinned manifest.

The normative comparison is
[extension-hook-parity.md](../features/extension-hook-parity.md). “Extensions
v2” is the roadmap name, not a schema-major bump.

### Terms

| Term | Exact meaning |
| --- | --- |
| observe | Receive a bounded normalized event without changing the operation |
| transform | Replace submitted prompt text before persistence or transmission |
| block | Reject submitted prompt text before persistence or transmission |
| rewrite | Replace a proposed native tool argument object before validation |
| deny | Produce a sticky negative decision; the operation cannot execute |
| resolve | Answer a real outstanding permission request with allow-once, deny, or abstain |
| annotate | Add attributed visible metadata without changing the original result |
| replace | Select a bounded attributed result for the next model request while retaining the original |
| continue | Start one later provider turn after a candidate agent end |
| stop | Request cancellation and continue draining to a terminal boundary |

`tool_start` remains observational and is never redefined as pre-execution.
`pre_tool_use` is the distinct synchronous control boundary.

### Frozen event vocabulary

Existing v1 names retain their current meaning:

```text
session_start session_end
before_agent_start agent_start agent_end agent_settled
text_delta thinking plan status steer_rejected
tool_start tool_progress tool_end permission_request
usage turn_end error custom_message user_message
```

The following additive names are contracted for #55 and the provider lanes:

```text
user_prompt_submit
turn_start
message_start message_update message_end
pre_compact post_compact compact_failed
model_change effort_change instructions_change workspace_change
subagent_start subagent_end
pre_tool_use
post_tool_use post_tool_failure post_tool_batch
provider_request provider_response
```

`provider_request` and `provider_response` contain only bounded redacted
metadata. They never contain raw headers, authorization values, cookies,
tokens, environment values, or raw request/response bodies.

`agent_start` and `agent_end` are candidate-iteration pairs. An
extension-requested continuation emits another pair. `agent_settled` is the
single final boundary for the top-level user request. This documents the
shipping behavior and resolves the ambiguous “once per prompt” wording in ADR
0027 without changing v1 traces.

### Frozen action vocabulary

The v1 actions remain `none`, `context`, `continue`, and `stop`. These additive
action kinds and Python constructors are frozen:

| Kind | Python constructor | Accepted event | Payload |
| --- | --- | --- | --- |
| `prompt_transform` | `transform_prompt(prompt)` | `user_prompt_submit` | replacement `prompt` |
| `prompt_block` | `block_prompt(reason)` | `user_prompt_submit` | visible `reason` |
| `tool_rewrite` | `rewrite_tool(arguments)` | `pre_tool_use` | JSON-object `arguments` |
| `tool_deny` | `deny_tool(reason)` | `pre_tool_use` | visible `reason` |
| `permission_decision` | `decide_permission(decision, reason)` | `permission_request` | `allow_once`, `deny`, or `abstain` |
| `tool_annotate` | `annotate_tool(content, display)` | `post_tool_use`, `post_tool_failure` | attributed `content` |
| `tool_result_replace` | `replace_tool_result(content, is_error)` | `post_tool_use`, `post_tool_failure` | replacement `content` plus error flag |

The Python types may exist before the selected provider implements their
effect. Returning a known but unavailable or unsupported action produces a
typed visible diagnostic and behaves as `none`; it is never approximated.
Unknown action kinds produce `invalid_action` and behave as `none`.

### Capability vocabulary

Every provider matrix contains these independent keys:

```text
extensions.prompt.observe
extensions.prompt.transform
extensions.prompt.block
extensions.lifecycle.session.observe
extensions.lifecycle.turn.observe
extensions.lifecycle.message.observe
extensions.lifecycle.compaction.observe
extensions.lifecycle.model.observe
extensions.lifecycle.effort.observe
extensions.lifecycle.instructions.observe
extensions.lifecycle.workspace.observe
extensions.lifecycle.subagent.observe
extensions.tool.pre.observe
extensions.tool.pre.rewrite
extensions.tool.pre.deny
extensions.permission.observe
extensions.permission.allow_once
extensions.permission.deny
extensions.permission.abstain
extensions.tool.post.observe
extensions.tool.post.annotate
extensions.tool.post.replace
extensions.tool.batch.observe
extensions.provider.request.observe_redacted
extensions.provider.response.observe_redacted
extensions.agent.continue
extensions.agent.cancel
extensions.project_local.discover
extensions.project_local.trust
```

Each entry has one current state:

- `supported`: the selected runtime implements the contracted effect now;
- `unsupported`: the provider/product boundary cannot provide the effect;
- `unavailable`: the vocabulary is contracted but this build/runtime has not
  implemented or enabled it.

Stable reason codes explain the state: `implemented`,
`contracted_not_implemented`, `provider_owned`, `protocol_missing`, and
`unknown_provider_or_capability`. Observational-only behavior is represented
without ambiguity: the relevant `.observe` key is supported while mutation or
decision keys are unsupported. The comparison manifest separately uses
`observe_only_host_owned` as a hook classification.

`tny doctor --json` returns the complete matrix and selected provider without
starting Python, a provider process, or a network connection. Endpoint health
probes are skipped in JSON mode; local presence/configuration checks remain.
The same immutable matrix is sent in the extension host's setup handshake.

Python receives frozen `CapabilityView`, `ProviderCapabilities`, and
`CapabilityEntry` values through `ExtensionAPI.capabilities`. Unknown keys,
unknown state strings, and optional fields are retained. The setup-time
`selected_provider` is a snapshot. In a long-lived TUI after a provider
switch, `event.provider` is authoritative and
`api.capabilities.for_provider(event.provider)` selects the immutable matrix.

### Version compatibility

The private host protocol remains major `1`. Event/action schema major remains
`1`; capability schema begins at major `1`. Initialization negotiates all
three majors explicitly. A legacy protocol-1 initialize request or response
without the schema object is interpreted as v1. An explicit incompatible
major fails extension initialization visibly and leaves the agent running
without hooks.

Compatibility rules:

- New event names, capability keys, optional event fields, and additive action
  kinds do not change a major.
- Existing event and action semantics cannot change in place.
- Unknown events become `UnknownEvent`; unknown fields remain in `payload`.
- Unknown capability names and state strings remain queryable.
- Unknown actions remain invalid rather than being guessed.
- Removing or changing a required field, action effect, ordering guarantee, or
  capability meaning requires a new schema major and explicit migration.

### Ordering and folding

The portable order is:

```text
session_start
user_prompt_submit -> fold prompt actions -> persist/send
before_agent_start -> agent_start
turn_start
  message_start -> message_update/deltas -> message_end
  pre_tool_use -> fold rewrite/deny -> validate rewritten arguments
    -> classify permission -> permission_request? -> fold decision
    -> tool_start -> tool_progress -> tool_end
    -> post_tool_use | post_tool_failure
  post_tool_batch
  provider_request/provider_response surround only the owned/observed wire edge
turn_end
agent_end -> continue?
agent_settled
session_end
```

Compaction is a separate atomic sequence:

```text
pre_compact -> post_compact | compact_failed
```

Listeners run in extension-name order, then registration order. Every matching
listener is invoked before folding. Precedence, highest first, is:

1. user cancellation or `stop`;
2. explicit prompt/tool/permission deny;
3. prompt/tool/result transform, rewrite, or replacement — last valid action
   in listener order wins;
4. permission allow-once;
5. annotations and context in listener order;
6. abstain, `none`, and hook failure.

A successfully folded deny is sticky. A later timeout, exception, host death,
or host restart cannot undo it. Consumed mutating actions are identified and
must not replay after restart. Extension actions never create persistent
permission grants or raise the configured permission mode.

### Failure policy

Discovery, import, handler, timeout, malformed response, and unavailable
runtime failures are bounded visible diagnostics and fail open. They do not
change an agent outcome and do not suppress later listeners. Explicitly folded
deny/block decisions remain fail closed for their exact operation. A denied
native tool call never reaches validation side effects, permission grants,
MCP, shell, subagent, or tool execution.

### Ownership and provider truth

- Native OpenAI: tny may implement prompt control, pre-tool rewrite/deny,
  permission resolution, result annotation/replacement, and redacted provider
  metadata in #55.
- Codex and ACP: prompt boundaries are tny-owned; permission decisions are
  supported only through real host requests. Tool rewrite/deny and result
  replacement remain unsupported unless a pinned protocol adds a decision
  surface.
- Cursor: the pinned bridge has no per-call permission decision surface.
  Permission observation and decisions, host-tool rewrite/deny, and result
  replacement are unsupported.
- Project-local discovery and trust remain unavailable until #59. Candidate
  discovery cannot execute code; trust cannot come from repo config.

The shipping #54 matrices intentionally marked #55 and adapter work
`unavailable`. #55 now reports the tny-owned shared lifecycle and native
OpenAI controls as supported. Provider-owned adapter and project-trust cells
remain unavailable or unsupported until their owning issue passes; downstream
issues do not rename keys or actions.

### Payload and secret limits

- normalized event JSON: 128 KiB;
- one host protocol line: 256 KiB;
- action/context/result text: 64 KiB;
- event/action name: 80 bytes;
- extension and custom-type name: 128 bytes;
- failure message: 512 bytes; debug traceback: 4 KiB.

Provider-native payloads do not cross the extension boundary. Capability data
is static and contains no credentials, URLs with embedded credentials,
headers, bodies, tokens, command environments, or provider stderr.

### Exclusions

This contract does not add:

- Pi tool, command, provider, renderer, UI, keybinding, or resource
  registration;
- Claude tasks, teams, worktrees, file/config watchers, prompt expansion,
  setup hooks, or directory-change behavior where tny lacks the operation;
- Pi fork/tree navigation where tny lacks the operation;
- raw header/body observation or arbitrary provider request mutation;
- hidden unattributed system/developer-context mutation;
- host-owned control without a pinned decision surface;
- project-local execution without #59 trust;
- Python execution in wasm or extension authority in libtny ABI 0.

### #55 implementation

The shared runtime applies prompt actions before persistence/send and maintains
logical session, turn, message, compaction, selection, instruction, workspace,
native subagent, candidate-end, and single-settlement ordering across engine
rebinds. New/resume/clear/recovery reasons and previous-session identity are
explicit where the corresponding operation exists.

The native backend calls a guarded runtime control seam at quiescent boundaries,
never from an emitted-event callback. Pre-tool folding precedes parsing and
schema validation; permission classification uses the effective canonical tool
and every target; extension allow-once never calls `perm_grant`; post-tool
folding precedes provider result persistence; batch observation precedes the
next request. Provider-declared parallel calls retain their IDs but execute and
fold serially in stable order.

Original and effective tool arguments/results are separate top-level audit
fields. Only schema-valid rewritten arguments become provider-history truth,
and only the bounded effective result reaches the next model request. A
persist failure stops before a later POST. Cancellation finalizes every
remaining call through failure/post/batch boundaries, and frontend signal probes
are rechecked after every blocking Python control call before any side effect.

Native `provider_request`/`provider_response` events use an allowlist and pair
every physical HTTP attempt with a stable logical request ID and attempt number.
Raw URL/header/body/error/cookie/credential data never enters the event. Raw
HTTP and SSE error bodies are not echoed into diagnostics.

## Consequences

- Provider lanes share names and can be implemented in parallel after #55.
- Capability output distinguishes not-yet-implemented work from impossible
  host control and prevents false parity claims.
- Python extensions can branch on provider truth without probing endpoints or
  importing provider schemas.
- The static descriptor adds no public libtny symbol and no network/startup
  work. Python remains optional and lazy.

## Verification gates

- C fixtures cover every provider matrix, schema negotiation, known/unknown
  actions, typed diagnostics, limits, and side-effect-free queries.
- Python fixtures cover immutable views, setup/runtime access, v1
  compatibility, and unknown keys/states.
- Doctor sentinel fixtures prove zero provider/Python execution and no secret
  values in output.
- The manifest verifier proves each pinned source hook appears exactly once
  and every public name has one normative definition.
- `make test`, size, wasm clean-unavailable behavior, libtny exports, and
  extension-free startup gates remain green.

## Local verification (2026-08-25)

Apple Silicon macOS, pre-roadmap baseline `c0592d7` versus #54 result
`fbbd3f2`, both built stripped with `make release`:

| Gate | Baseline | #54 result |
| --- | --- | --- |
| stripped `build/tny` | 545,472 bytes | 545,664 bytes (+192) |
| `tny --version`, hyperfine 50 runs | 2.2 ± 0.6 ms | 2.2 ± 0.5 ms |
| extension-free `status --json`, hyperfine 50 runs | 1.9 ± 0.4 ms | 2.2 ± 0.5 ms |

Both startup paths remain under the 5 ms command budget; the sub-5-ms
hyperfine warning applies, so these numbers are a regression gate rather than
a speedup claim. The sentinel integration additionally proved that
extension-free status and JSON doctor execute neither Python nor provider
binaries.

The full native suite passed 204 C tests plus 18 Python extension tests and all
fixture integrations. `make size-check` and the C/Python libtny consumers
passed. The focused capability mutation target killed all 11 valid mutants
(six generated boolean mutants were uncompilable). `emcc` was not installed
locally, so the required wasm compile/size evidence is delegated to the
repository's emsdk CI lane rather than claimed from source inspection.
