# Optional ACP agents

ACP runs an external **agent**, with its own inference loop. tny remains the
owner of its session runner, tools, permissions and user interface. The external
agent receives a stdio MCP server exposing only `run_code`; nested discovery
and execution use the current permission-filtered tny catalog.
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
      "claude": { "command": ["claude-agent-acp"], "model": "sonnet" }
    }
  }
}
```

Use `tny --provider acp:claude ask '...'` or select `acp:claude` in the TUI.
Model turns currently require the verified
`@agentclientprotocol/claude-agent-acp` version `0.75.1`. Other adapter
identities or versions fail closed. Catalog discovery is not a promise that an
adapter is admitted for execution. Use an absolute executable path or a PATH
command; relative paths containing a slash are rejected.

The requested model must appear in the adapter's session catalog. tny confirms
`session/set_config_option`'s returned selection (or the older `set_model`
acknowledgement) before sending the prompt. Unknown, unavailable, rejected or
unconfirmed models fail explicitly. Use an exact advertised ID such as
`sonnet`; an account's default model can be different and more expensive.
`tny --provider acp --agent claude-agent-acp models` lists the adapter's
catalog, not Claude Code's CLI catalog. If Claude Code shows a newer model
but tny does not, check `claude-agent-acp --version` and update the adapter
(e.g. `mise install 'npm:@agentclientprotocol/claude-agent-acp@latest'` when
installed with mise). Then run `tny ... models` again. Updating Claude Code
alone does not update the adapter or its bundled Claude Agent SDK. Model names
may change while selectable IDs remain aliases (for example, `opus[1m]`).
Reasoning effort is selectable only when the agent advertises a compatible
`thought_level` option and confirms it. Fast service tier is not an ACP field.

## Tools and policy

The bridge exposes the same singleton `run_code` schema as native HTTP.
Bounded Lua discovers and calls the filtered native catalog through
`tools.list`, `tools.describe` and `tools.call`. Direct MCP calls to native tool
names are rejected. Nested operations retain prepared validation, permissions,
intercepts and result limits. See [ADR 0174](../adr/0174-execution-server-code-mode.md).

Every admitted turn uses the verified Claude adapter's tools-only metadata
(`tools: []`, `settingSources: []`, `strictMcpConfig: true`). Standard ACP fs and
terminal callbacks are not advertised. The adapter starts in a private scratch
directory and receives that directory as ACP cwd. Workspace operations use the
execution server's trusted snapshot; under `--ssh`, they use the existing remote
seam. Reported adapter tool notifications are suppressed in favor of tny's
execution events. Unknown adapters cannot retain unmediated built-in tools.

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
binary. The bridge cannot assume the embedding process is tny. Library-hosted agent tool execution and pointer-bearing custom tools cannot
currently enter the execution child. A matching bridge executable does not
enable that execution path; it fails closed. Consult the [evidence ledger](../verification/execution-code-mode/evidence.md)
for the verified embedding coverage.

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
