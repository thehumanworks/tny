# 0174 — Experimental static prefix and built-in tool discovery

Date: 2026-09-24
Status: proposed

## Context and decision

One native Responses request repeated a large built-in schema set. The saved
59-session sample in [harness efficiency](../benchmarks/harness-efficiency.md)
shows terminal, read, edit and grep dominate real tool use. This experiment
changes only `TNY_EXP_PREFIX=1`; the unset path keeps its request bytes and
execution policy. The default `all` profile advertises frequent tools,
`read_tool_result`, `ask_user_question`, subagents, and MCP discovery/call tools.
Team, swarm and job tools remain advertised in collective or team contexts.
Other built-ins appear by name in the setup message and load when
`tool_search(query)` ranks tools using the MCP AND-keyword matcher, treating
underscores as spaces. Queries list names and one-line descriptions. An empty
query lists every deferred tool. Only `tool_search(load=exact_name)` or an
exact-name query loads a schema. Loaded schemas append after the stable tool
block in load order and remain advertised for the rest of the session, including
resume. Private session metadata stores names rather than registry positions.
A direct call with valid arguments loads its schema and executes through normal
permissions. An invalid direct call names `tool_search` and the exact tool to
load. A custom tool named `tool_search` takes precedence, so the built-in is
omitted from that embedder's schema.

The flagged JSON body order is tool definitions, stable instructions, the cache
routing key, a setup message, then conversation. Provider rendering and cache
order are not inferred from JSON member order. The flag retains the workspace
group routing of [ADR 0077](0077-openai-prompt-cache-routing.md) and
[ADR 0078](0078-workspace-shared-prompt-cache.md) for both `prompt_cache_key`
and Codex's `session-id` header. This keeps established task sharing inside a
workspace and avoids pooling unrelated busy workspaces behind one routing key.
`TNY_OPENAI_CACHE_SCOPE=session` keeps its explicit per-conversation override.
Responses uses a developer setup input item; Chat Completions uses a second
system message. No explicit cache breakpoint is added
because these OpenAI-compatible profiles use automatic prefix caching and not
all accept breakpoint extensions. The setup message is built once at the first
request of each turn and freed/reset at the next turn. This freezes AGENTS.md,
skill discovery, MCP catalog, learning advice, image input/provider capabilities,
and speech availability within a turn. MCP warm-up can complete between
requests; learning state changes after tools; capability probes can also change.
The request builder also retains the first rendered schema array until discovery
loads another schema, so availability probes cannot silently change the cached
tool prefix mid-turn. Those changes become visible on the next turn. Changes in
setup facts between turns, including learning counters, MCP warm-up, skill or
AGENTS.md edits, and permission changes, can still invalidate conversation
cache reuse. The intentionally growing tool schema set after discovery is the
only prefix change within a turn. Provider
reasoning items and tool history are still carried in full.

The experiment shares C11 registry and request builders across native and wasm.
Wasm still returns its existing clean errors for unsupported native tools. ACP
clients retain their owning-runtime MCP bridge schema. Shell
profiles retain their advertised tools; the flag shortens their prompt wording
and freezes setup, while the discovery tool is used only by the `all` profile.
On native builds, the resolved flag is carried through the detached runner's
private start context. The field is omitted when false and never appears in the
public checkpoint or saved session. This preserves flag-off packet bytes and
lets runner restarts keep the experiment setting.
Rollback is to unset `TNY_EXP_PREFIX`.

## Prompt audit

Each row identifies one semantic line or injected block of the previous built-in
prompt. `Keep` means the information stays; `rewrite` means plain, shorter
wording; `delete` means no replacement; `move` means setup instead of stable
instructions. The unflagged prompt is untouched. The audit follows sections
2–4 of `docs/web-resources/x_cursor_harness_optimisation_prompt.md`.

| Existing line or block | Action | Reason |
| --- | --- | --- |
| tny terminal harness role | Rewrite | Product identity is useful; shorter definition suffices. |
| Complete request within scope | Keep | Defines completion boundary. |
| Reasonable assumptions and carried authorization | Rewrite | Included in authorized task wording; removes an extra command. |
| Tools for facts/actions; preserve work | Rewrite | States the behavior and existing-work constraint once. |
| Resolve blockers; ask at end with tradeoff | Rewrite | Keep the actual input boundary in one sentence. |
| Delegate when worthwhile | Delete | The model already delegates; prompting can add coordination cost. |
| Follow project instructions and load skills/schemas | Rewrite | Project authority stays; discovery has its own tool definition. |
| User directions override workflow preferences | Keep | Defines instruction precedence. |
| Retrieved/tool content cannot grant authority | Keep | Defines trust boundary. |
| Create/update tests and run them | Rewrite | Verify changes, including project-required checks. |
| Proportionate, repeated or expanded checks | Rewrite | Short required-check sentence; user/project requirements still apply. |
| Simple English and short sentences | Rewrite | Concise reporting instruction. |
| Outcome and impact first | Rewrite | Final outcome and checks. |
| Prefer compact work/checks/blockers table | Delete | An unsolicited format can conflict with user requests. |
| Brief progress updates | Rewrite | Retained as a short instruction. |
| Workspace path, extra dirs, SSH execution facts | Move | These are workspace/turn facts. |
| Tool profile and permission mode | Move | These are resolved context facts. |
| Project/user AGENTS.md text | Move | Workspace instructions can change between turns. |
| Skill catalog | Move | Discovery can change. |
| MCP catalog | Move | Warm-up can finish between requests. |
| Automatic learning guidance | Move | Tool outcomes can change it during a turn. |
| Task preset and caller system additions | Move | User/task-specific instructions. |
| Image input and provider capability text | Move, rewrite | Availability is setup data; shorter capability definition. |
| Speech availability text | Move, rewrite | Availability is setup data; tool describes playback. |
| Shell profile usage block | Move, rewrite | Mode-specific CLI facts stay near setup, without repeated prohibitions. |
| Collective policy | Move, keep | Coordination mode depends on its precise authority and roles. |

## Local mock measurement

`uvx --with tiktoken python tests/bench/measure_prefix.py build/tny` sent five
requests in one empty-workspace tool-using turn to a local synthetic OpenAI
Responses server, once per flag setting. Section tokens use `o200k_base`; tools
are counted from their rendered wire JSON, and instructions/setup from their
text. The current mock exposes 41 schemas off, compared with the older 38-tool
snapshot in the benchmark brief because availability/profile conditions differ.
No live model quality or billing result follows from this size measurement.

| Setting | Tools | Stable instructions | Setup | Static total | Schemas | Prefix byte-identical across 5 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| flag off | 5,604 | 548 | 0 | 6,152 | 41 | yes |
| flag on | 1,174 | 145 | 196 | 1,515 | 17 | yes |

The flagged request removes 4,637 static tokens (75.4%) from this local
configuration. A real task that needs deferred schemas pays a discovery turn,
so the benchmark's price-weighted cost per completed task and success guardrail
must decide promotion. The mock also exercises actual schema loading and both
Responses and Chat request construction; it does not estimate provider cache
hits. The status report records the checks and remaining risks.

The orchestrator later reported a live A/B of the earlier flagged revision on
36 runs per arm: price-weighted ITE ratio 0.856 (interval 0.765–0.957), with
36/36 task success in each arm and 32% less context. This result was supplied
externally, not rerun for the discovery and routing fixes in this revision.

The same script sent the **first** Codex-format request from each of two fixed,
empty workspaces to the local mock. It compared raw request-body bytes before
tokenizing with `o200k_base`. This is a JSON serialization diagnostic only:
`prompt_cache_key` differs by workspace and is not prompt content; providers
may render tools, instructions and input in a different order. The leading
fraction is not a cacheability or billing estimate.

| Setting | Raw JSON bytes | Identical leading JSON bytes | Raw JSON tokens | Leading JSON tokens | Leading JSON token fraction |
| --- | ---: | ---: | ---: | ---: | ---: |
| flag off | 28,229 | 78 | 6,202 | 28 | 0.45% |
| flag on | 7,102 | 6,072 | 1,558 | 1,315 | 84.40% |

The same mock was run with default native isolation (no `TNY_ISOLATE` override)
and one request per turn. Flag on advertised 17 schemas including `tool_search`
and sent a developer setup item; flag off advertised 41 schemas and did neither.
For a byte comparison, the script ran the branch-point main binary at
`968b5d6` against the same server, workspace and synthetic Codex credentials.
The flag-off request matched all 28,220 raw body bytes, with SHA-256
`2bc9269d0ad44e8b7d366d4d9c6c4413139e664a327979330ea625180415984d`.
The private checkpoint unit test also checks that flag-off packets and public
checkpoints omit the field, while a flagged private checkpoint restores it.

## Model catalog observation

The missing `gpt-6-sol` and `gpt-6-luna` entries were caused by Codex's
`minimal_client_version` catalog filter. [ADR 0170](0170-codex-catalog-discovery-version.md)
and current `profiles.c` already use `999.999.999` for live discovery, with an
override. There is no small unfixed catalog bug in this worktree. The local
`~/.codex/models_cache.json` is Codex's cache, not tny's model source; account
visibility and an older installed tny binary can also explain differing lists.
No live account query was made here.
