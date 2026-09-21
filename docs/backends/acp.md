# Optional ACP agents

ACP runs an external **agent**, with its own inference loop. tny remains the
owner of its session runner, tools, permissions and user interface. The external
agent receives a stdio MCP server containing the current tny tool registry.
Native HTTP remains the default and requires no vendor executable.

```sh
# Authenticate separately with Claude's own account login, then:
tny --provider acp --agent claude-agent-acp --model sonnet ask 'Read README.md'

# Literal executable and argv: no shell evaluation.
tny --provider acp --agent /absolute/path/to/adapter -- --adapter-flag -- ask 'Hello'
```

Reusable settings:

```json
{
  "acp": {
    "agents": {
      "claude": { "command": ["claude-agent-acp"], "model": "sonnet" },
      "pi": { "command": ["pi-acp"] }
    }
  }
}
```

Use `tny --provider acp:claude ask '...'` or select `acp:claude` in the TUI.
The executable name `pi-acp` is an example, **not a compatibility guarantee**:
choose an adapter implementing ACP client-supplied `mcpServers`. In particular,
[`regadas/pi-acp`](https://github.com/regadas/pi-acp#limitations) rejects nonempty `mcpServers`, so it cannot use this bridge.
The [`@geohar/pi-acp`](https://pi.dev/packages/@geohar/pi-acp) adapter documents MCP forwarding via a separately installed
`pi-mcp-adapter`; actual compatibility requires testing the installed versions.
No pi live validation is claimed here.

The requested model must appear in the adapter's session catalog. tny confirms
`session/set_config_option`'s returned selection (or the older `set_model`
acknowledgement) before sending the prompt. Unknown, unavailable, rejected or
unconfirmed models fail explicitly. Use an exact advertised ID such as
`sonnet`; an account's default model can be different and more expensive.
Reasoning effort is selectable only when the agent advertises a compatible
`thought_level` option and confirms it. Fast service tier is not an ACP field.

## Tools and policy

The bridge converts **the current native schema**, including custom SDK tools,
into MCP `tools/list`. Calls use the same prepared-tool validation, permissions,
intercepts, tool dispatch, result bounds and custom async completion as HTTP.
Imported MCP servers are reachable through tny's normal MCP tools. Hooks cover
bridge pre-tool, permission, post-tool and batch boundaries. Provider request
hooks cannot observe the external agent's model connection. Generic agents retain their reported tool events. Verified Claude tools-only
mode suppresses duplicate adapter notifications; tny MCP executions are authoritative.

For the recognized Claude adapter, tny requests a tools-only agent using its
vendor metadata (`tools: []`, `settingSources: []`, `strictMcpConfig: true`). The standard ACP fs and
terminal callbacks are deliberately not advertised: all such operations use
tny's native tool bridge. Unknown agents may retain built-in tools; those tools
are outside tny's schema, intercept and path-rule enforcement. Their reported
ACP permission requests are still handled by tny's selected permission mode.

`--ssh` is enabled only for the specifically verified Claude adapter version
`0.75.1`, whose metadata disables built-in tools and settings. The adapter starts
in a private local scratch directory and receives that directory as ACP cwd;
workspace tools and project AGENTS.md use the existing remote tny seam. Generic
or unverified adapters fail closed. This version gate avoids promising remote
confinement from an extension a different adapter might silently ignore.

## Sessions, embedding and limits

Subprocesses start only for actual agent use, never help/version. The detached
runner owns the bridge and adapter, so caller detachment does not end the turn.
Ordinary repeated turns reuse the external session; guarded managed turns stop
the owned adapter after completion and use capability-gated load when continuing. Resume after restart requires
advertised `loadSession`; tny sends the bridge with both new and load requests.
An adapter lacking load support gets an actionable error, not a fresh session
presented as resumed.

For C/Python/Node embedding, configure the ACP command explicitly and set
`TNY_ACP_BRIDGE_EXECUTABLE` to an absolute executable path of the matching tny
binary. The bridge cannot assume the embedding process is tny. Custom tools
remain in the embedding runtime; the relay never reconstructs them in a child.
Only owner-thread event-loop dispatch invokes their callbacks. Async completion
and cancellation use the existing public custom-tool contract.

Native mid-turn checkpoint restart, steering, compaction, generated-image preview
continuation and automatic recovery-policy learning are not exposed by this
client. Image prompts require the advertised image capability; native read_image
results deliver captured bytes as MCP image content, with an explicit delivery
limit. Usage/cost include only what the adapter reports. wasm returns an explicit
unsupported diagnostic. No optional HTTP/SSE MCP or WebSocket agent transport is
advertised.

See the [capability and verification matrix](../verification/acp-client/README.md)
and [ADR 0164](../adr/0164-optional-acp-clients.md) for authority/resource bounds.

Managed jobs, DAGs, shared admission, read-only teams and collective/manifest
swarms can request verified Claude ACP children. Every launch, including ordinary non-DAG jobs, carries a required
tools-authority constraint: after the owned launch is counted, the child checks
the actual adapter identity/version before session/new or load. Parsing, help
and status never launch a verifier. Guarded children initialize in private
scratch space while tny's tools retain the intended workspace. Read-only ceilings
still precede yolo and permission rules.

Per-item ACP profiles use a separate selector branch without weakening native
HTTP credential checks. Named and custom command argv, model/effort and inherited
authority are frozen; queued/retried launches do not reread mutable profile
commands. Retry scope comparisons include ACP configuration. Adapter executable
bytes and external account login remain mutable outside this snapshot contract.

Cleanup-sensitive attempts require proof that the owned adapter/wrapper/relay
tree stopped. A fresh private receipt records successful cleanup; missing or
uncertain proof retains admission/workspace holds. Admission counts launch
attempts, not account-wide model requests, tokens or money.

A positive --max-steps maps to verified Claude SDK maxTurns: conversation turns,
not tny's native inference-loop calls. Other adapters reject this hard limit
unless supported. CLI steps is null for ACP because actual model-call counts
are unknown. Ordinary local subagents preserve the same command/authority
snapshot; existing native platform, SSH, embedding and nested-enrollment
constraints remain applicable.
Synchronous native tool calls cooperate with owner cancellation; an agent-only
MCP cancellation notification is handled after that synchronous call returns.

Verified Claude uses adapter bypassPermissions in every tny permission mode:
strict MCP configuration leaves tny prepared-tool rules as the sole authority.
The adapter's duplicate tool notifications are suppressed in this verified mode;
generic agents retain their external tool observations.

ACP cost updates carry the reported currency and cumulative session scope. The
latest update replaces the total; it must not be summed over events or turns.
Context occupancy is not input/output token usage. Those counts remain unknown
in CLI and managed-job summaries; job usage remains unknown rather than known
zero. SDK events mark `tokens_reported`/`tokensReported` false.
The frozen C event view retains zero placeholders and additive metadata getters
provide currency, cumulative scope and token-reporting status. Workflow totals
exclude cumulative session costs rather than risk counting the same session twice.

Swarm activation and subscribed team messages are durably queued with their
receipt/activation state and forwarded at ACP prompt boundaries. A successful
end-turn acknowledgement clears only the forwarded prefix; errors and
cancellation retain it for retry/resume. The forwarding queue is bounded to
256 messages and 1 MiB. ACP does not expose the external agent's internal
model-call boundaries for automatic mid-prompt mailbox injection. Native tny
team/mailbox tools remain available during a prompt.

Guarded launches require an absolute adapter executable or a PATH command;
relative executable paths containing a slash fail before spawn with guidance.
Use absolute paths for script/config arguments too, since guarded adapters run
in private scratch space. Settings command arrays preserve literal empty and
`--` arguments in frozen child snapshots.

Native search, image and speech services retain their existing provider and
credential requirements. A Claude account login does not supply credentials
for those independent HTTP services; configure them as described in their
native service guides.
