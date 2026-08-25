# Extension hook parity manifest

This is the normative, release-pinned comparison for
[ADR 0028](../adr/0028-extension-parity-contract.md). It measures semantic,
capability-scoped parity, not event-count equality or full plugin-system
parity. Captured 2026-08-25.

## Baselines

| Product | Normative release | Immutable source |
| --- | --- | --- |
| Pi | v0.84.3, commit `4e58f324fae8ebfa98a3d45181fb248072a2afac` | [`types.ts`](https://github.com/earendil-works/pi/blob/4e58f324fae8ebfa98a3d45181fb248072a2afac/packages/coding-agent/src/core/extensions/types.ts), [`extensions.md`](https://github.com/earendil-works/pi/blob/4e58f324fae8ebfa98a3d45181fb248072a2afac/packages/coding-agent/docs/extensions.md) |
| Codex | `rust-v0.149.1`, tag object `980a6d12110b110d29ec13bdcbe14011100b3566`, commit `ff29a44391deccde0aba0f8390337d7f3c319ea4` | [`HookEventName`](https://github.com/openai/codex/blob/ff29a44391deccde0aba0f8390337d7f3c319ea4/codex-rs/protocol/src/protocol.rs#L1502-L1514), tagged app-server schemas |
| Claude Code | npm `2.1.245`, shasum `cceab6b3a7a4d899e2a94963852304aaba43d6ac` | [official hooks reference](https://code.claude.com/docs/en/hooks), npm integrity recorded in [sources.md](../sources.md) |
| fx | v0.0.5, commit `df7e6245e1992758d4060c97477ceafa27770551` | [`definitions.zig`](https://github.com/vercel-labs/fx/blob/df7e6245e1992758d4060c97477ceafa27770551/src/core/hooks/definitions.zig) |

The fx `main` observation at
`16eda256ca3c94a50744a5fb57d033ec18011f24` has the same four hook definitions
but is non-normative. Codex alpha/main may add `Interrupt`; it is labeled an
observation rather than silently added to the stable baseline.

## Classifications

Every relevant source hook has exactly one classification:

- `equivalent`: same material lifecycle/effect and compatible payload;
- `equivalent_renamed`: same material lifecycle/effect under normalized names;
- `observe_only_host_owned`: reliable observation exists, but the provider
  owns execution and exposes no equivalent control;
- `unsupported_safety`: tny deliberately refuses the behavior;
- `operation_absent`: tny lacks the underlying product operation.

`implemented` means shipping at the #54 baseline. `contracted` means the name
and semantics are frozen but its owning downstream issue must still turn the
capability from `unavailable` to `supported`. `deferred` is a deliberate
exclusion, not hidden work.

## Pi v0.84.3

| Source hook | tny event/action | Classification | Capability keys | Authority | Status / lane |
| --- | --- | --- | --- | --- | --- |
| `project_trust` | project-local trust state | `equivalent_renamed` | `extensions.project_local.trust` | tny | contracted #59 |
| `resources_discover` | — | `operation_absent` | — | product absent | deferred |
| `session_start` | `session_start` | `equivalent` | `extensions.lifecycle.session.observe` | tny | implemented |
| `session_info_changed` | — | `operation_absent` | — | product absent | deferred |
| `session_before_switch` | `session_end` then `session_start` with reasons | `equivalent_renamed` | `extensions.lifecycle.session.observe` | tny | contracted #55 |
| `session_before_fork` | — | `operation_absent` | — | product absent | deferred |
| `session_before_compact` | `pre_compact` | `equivalent_renamed` | `extensions.lifecycle.compaction.observe` | tny | contracted #55 |
| `session_compact` | `post_compact` | `equivalent_renamed` | `extensions.lifecycle.compaction.observe` | tny/host | contracted #55/#56 |
| `session_compact_failed` | `compact_failed` | `equivalent_renamed` | `extensions.lifecycle.compaction.observe` | tny/host | contracted #55/#56 |
| `session_shutdown` | `session_end` | `equivalent_renamed` | `extensions.lifecycle.session.observe` | tny | implemented payload expansion #55 |
| `session_before_tree` | — | `operation_absent` | — | product absent | deferred |
| `session_tree` | — | `operation_absent` | — | product absent | deferred |
| `context` | `instructions_change` | `equivalent_renamed` | `extensions.lifecycle.instructions.observe` | tny | contracted #55 |
| `before_provider_request` | `provider_request` redacted metadata | `equivalent_renamed` | `extensions.provider.request.observe_redacted` | tny/native or host-observed | contracted #55/#56-#58 |
| `before_provider_headers` | redacted metadata only; no raw headers | `unsupported_safety` | `extensions.provider.request.observe_redacted` | safety boundary | deferred |
| `after_provider_response` | `provider_response` redacted metadata | `equivalent_renamed` | `extensions.provider.response.observe_redacted` | tny/native or host-observed | contracted #55/#56-#58 |
| `before_agent_start` | `before_agent_start` | `equivalent` | `extensions.prompt.observe` | tny | implemented |
| `agent_start` | `agent_start` | `equivalent` | `extensions.lifecycle.turn.observe` | tny | implemented event; full lifecycle #55 |
| `agent_end` | `agent_end` | `equivalent` | `extensions.agent.continue` | tny | implemented |
| `agent_settled` | `agent_settled` | `equivalent` | `extensions.lifecycle.turn.observe` | tny | implemented |
| `turn_start` | `turn_start` | `equivalent` | `extensions.lifecycle.turn.observe` | tny/host | contracted #55/#56-#58 |
| `turn_end` | `turn_end` | `equivalent` | `extensions.lifecycle.turn.observe` | tny/host | implemented |
| `message_start` | `message_start` | `equivalent` | `extensions.lifecycle.message.observe` | tny/host | contracted #55/#56-#58 |
| `message_update` | `message_update` plus existing deltas | `equivalent` | `extensions.lifecycle.message.observe` | tny/host | contracted #55/#56-#58 |
| `message_end` | `message_end` | `equivalent` | `extensions.lifecycle.message.observe` | tny/host | contracted #55/#56-#58 |
| `tool_execution_start` | `tool_start` | `equivalent_renamed` | `extensions.tool.pre.observe` | tny/host | implemented observation; synchronous seam #55 |
| `tool_execution_update` | `tool_progress` | `equivalent_renamed` | `extensions.tool.post.observe` | tny/host | implemented |
| `tool_execution_end` | `tool_end` | `equivalent_renamed` | `extensions.tool.post.observe` | tny/host | implemented |
| `model_select` | `model_change` | `equivalent_renamed` | `extensions.lifecycle.model.observe` | tny | contracted #55 |
| `thinking_level_select` | `effort_change` | `equivalent_renamed` | `extensions.lifecycle.effort.observe` | tny | contracted #55 |
| `user_bash` | — | `operation_absent` | — | product absent | deferred |
| `input` | `user_prompt_submit`; `prompt_transform`/`prompt_block` | `equivalent_renamed` | `extensions.prompt.observe`, `.transform`, `.block` | tny | contracted #55 |
| `tool_call` | `pre_tool_use`; `tool_rewrite`/`tool_deny` | `equivalent_renamed` | `extensions.tool.pre.observe`, `.rewrite`, `.deny` | tny native; host observe-only | contracted #55/#56-#58 |
| `tool_result` | `post_tool_use`/`post_tool_failure`; annotate/replace | `equivalent_renamed` | `extensions.tool.post.observe`, `.annotate`, `.replace` | tny native; host observe-only | contracted #55/#56-#58 |

Pi's tool/command/provider/renderer/UI/keybinding/resource registration APIs
are plugin APIs rather than lifecycle hooks and are explicitly outside this
manifest.

## Codex rust-v0.149.1 lifecycle hooks

| Source hook | tny event/action | Classification | Capability keys | Authority | Status / lane |
| --- | --- | --- | --- | --- | --- |
| `PreToolUse` | observed `pre_tool_use`; no Codex tool rewrite/deny | `observe_only_host_owned` | `extensions.tool.pre.observe`, `.rewrite`, `.deny` | Codex host | contracted #56 |
| `PermissionRequest` | `permission_request`; allow-once/deny/abstain via live request | `equivalent_renamed` | `extensions.permission.observe`, `.allow_once`, `.deny`, `.abstain` | Codex request | contracted #56 |
| `PostToolUse` | `post_tool_use`/`post_tool_failure`, no result replacement | `observe_only_host_owned` | `extensions.tool.post.observe`, `.annotate`, `.replace` | Codex host | contracted #56 |
| `PreCompact` | `pre_compact` | `observe_only_host_owned` | `extensions.lifecycle.compaction.observe` | Codex host | contracted #56 |
| `PostCompact` | `post_compact`/`compact_failed` | `observe_only_host_owned` | `extensions.lifecycle.compaction.observe` | Codex host | contracted #56 |
| `SessionStart` | `session_start` plus thread lifecycle | `equivalent_renamed` | `extensions.lifecycle.session.observe` | tny/Codex | implemented; expansion #56 |
| `SessionEnd` | `session_end` plus thread close | `equivalent_renamed` | `extensions.lifecycle.session.observe` | tny/Codex | implemented; expansion #56 |
| `UserPromptSubmit` | `user_prompt_submit`; prompt transform/block before `turn/start` | `equivalent_renamed` | `extensions.prompt.observe`, `.transform`, `.block` | tny | contracted #55 |
| `SubagentStart` | `subagent_start` from collaboration items | `observe_only_host_owned` | `extensions.lifecycle.subagent.observe` | Codex host | contracted #56 |
| `SubagentStop` | `subagent_end` from collaboration items | `observe_only_host_owned` | `extensions.lifecycle.subagent.observe` | Codex host | contracted #56 |
| `Stop` | `agent_end`; continuation before final settlement | `equivalent_renamed` | `extensions.agent.continue`, `.cancel` | tny wrapper | implemented |

Stable Codex has no `Interrupt` hook. Alpha/main observations are not a release
claim and are reconsidered only when a stable release is deliberately pinned.

## Claude Code 2.1.245

| Source hook | tny event/action | Classification | Capability keys | Authority | Status / lane |
| --- | --- | --- | --- | --- | --- |
| `SessionStart` | `session_start` | `equivalent` | `extensions.lifecycle.session.observe` | tny | implemented |
| `Setup` | — | `operation_absent` | — | product absent | deferred |
| `UserPromptSubmit` | `user_prompt_submit`; prompt transform/block | `equivalent_renamed` | `extensions.prompt.observe`, `.transform`, `.block` | tny | contracted #55 |
| `UserPromptExpansion` | — | `operation_absent` | — | product absent | deferred |
| `PreToolUse` | `pre_tool_use`; native rewrite/deny, host observe-only | `equivalent_renamed` | `extensions.tool.pre.observe`, `.rewrite`, `.deny` | tny native/provider host | contracted #55/#56-#58 |
| `PermissionRequest` | `permission_request`; allow-once/deny/abstain where real | `equivalent_renamed` | `extensions.permission.observe`, `.allow_once`, `.deny`, `.abstain` | tny/provider request | contracted #55/#56/#57 |
| `PermissionDenied` | denied permission plus `post_tool_failure` | `equivalent_renamed` | `extensions.permission.deny`, `extensions.tool.post.observe` | tny/provider request | contracted #55/#56/#57 |
| `PostToolUse` | `post_tool_use`; native annotation/replacement | `equivalent_renamed` | `extensions.tool.post.observe`, `.annotate`, `.replace` | tny native/provider host | contracted #55/#56-#58 |
| `PostToolUseFailure` | `post_tool_failure`; native annotation/replacement | `equivalent_renamed` | `extensions.tool.post.observe`, `.annotate`, `.replace` | tny native/provider host | contracted #55/#56-#58 |
| `PostToolBatch` | `post_tool_batch` | `equivalent_renamed` | `extensions.tool.batch.observe` | tny/host | contracted #55/#56-#58 |
| `Notification` | bounded `status` | `equivalent_renamed` | `extensions.lifecycle.turn.observe` | tny/host | implemented |
| `MessageDisplay` | `message_update` plus `text_delta` | `equivalent_renamed` | `extensions.lifecycle.message.observe` | tny/host | contracted #55/#56-#58 |
| `SubagentStart` | `subagent_start` | `equivalent` | `extensions.lifecycle.subagent.observe` | tny/host | contracted #55/#56-#58 |
| `SubagentStop` | `subagent_end` | `equivalent_renamed` | `extensions.lifecycle.subagent.observe` | tny/host | contracted #55/#56-#58 |
| `TaskCreated` | — | `operation_absent` | — | product absent | deferred |
| `TaskCompleted` | — | `operation_absent` | — | product absent | deferred |
| `Stop` | `agent_end`; continuation/stop folding | `equivalent_renamed` | `extensions.agent.continue`, `.cancel` | tny | implemented |
| `StopFailure` | `error`, `turn_end`, `agent_end` | `equivalent_renamed` | `extensions.lifecycle.turn.observe` | tny/host | implemented; detail #55/#56-#58 |
| `TeammateIdle` | — | `operation_absent` | — | product absent | deferred |
| `InstructionsLoaded` | `instructions_change` | `equivalent_renamed` | `extensions.lifecycle.instructions.observe` | tny | contracted #55 |
| `ConfigChange` | — | `operation_absent` | — | product absent | deferred |
| `CwdChanged` | — | `operation_absent` | — | product absent | deferred |
| `DirectoryAdded` | `workspace_change` | `equivalent_renamed` | `extensions.lifecycle.workspace.observe` | tny | contracted #55 |
| `FileChanged` | — | `operation_absent` | — | product absent | deferred |
| `WorktreeCreate` | — | `operation_absent` | — | product absent | deferred |
| `WorktreeRemove` | — | `operation_absent` | — | product absent | deferred |
| `PreCompact` | `pre_compact` | `equivalent` | `extensions.lifecycle.compaction.observe` | tny/host | contracted #55/#56 |
| `PostCompact` | `post_compact` | `equivalent` | `extensions.lifecycle.compaction.observe` | tny/host | contracted #55/#56 |
| `Elicitation` | bounded `permission_request` where a provider exposes it | `equivalent_renamed` | `extensions.permission.observe`, `.allow_once`, `.deny`, `.abstain` | provider request | contracted #56/#57 |
| `ElicitationResult` | permission resolution/result observation | `equivalent_renamed` | `extensions.permission.observe` | provider request | contracted #56/#57 |
| `SessionEnd` | `session_end` | `equivalent` | `extensions.lifecycle.session.observe` | tny | implemented |

Claude project hooks are executable repository configuration. tny reaches
project scope only through #59's explicit hash-bound trust; it never imports
Claude hook configuration.

## fx v0.0.5

| Source hook | tny event/action | Classification | Capability keys | Authority | Status / lane |
| --- | --- | --- | --- | --- | --- |
| `PreToolUse` | `pre_tool_use`; native rewrite/deny | `equivalent_renamed` | `extensions.tool.pre.observe`, `.rewrite`, `.deny` | tny native | contracted #55 |
| `Stop` | `agent_end`; continuation before settlement | `equivalent_renamed` | `extensions.agent.continue` | tny | implemented |
| `PostTurnEnd` | `turn_end` then final `agent_settled` | `equivalent_renamed` | `extensions.lifecycle.turn.observe` | tny | implemented; full lifecycle #55 |
| `AttentionRequired` | `permission_request`/`status` | `equivalent_renamed` | `extensions.permission.observe` | tny/provider | implemented observation |

fx's four definitions are internal/runtime hooks; its public SDK event and
permission callbacks are not a user-loaded Python hook system.

## Current #54 provider matrices

The exact machine-readable matrices are emitted by `tny doctor --json` and
passed unchanged to `ExtensionAPI.capabilities`. At the #54 baseline:

| Provider | `supported` keys | `unsupported` keys | Every other key |
| --- | --- | --- | --- |
| Native OpenAI | prompt observe; session observe; permission observe; tool-post observe; agent continue/cancel | none | `unavailable` pending #55/#59 |
| Codex | prompt observe; session observe; permission observe; tool-post observe; agent continue/cancel | tool-pre rewrite/deny; tool-post annotate/replace | `unavailable` pending #55/#56/#59 |
| Cursor | prompt observe; session observe; tool-post observe; agent continue/cancel | tool-pre rewrite/deny; all permission keys; tool-post annotate/replace | `unavailable` pending #55/#58/#59 |
| ACP | prompt observe; session observe; permission observe; tool-post observe; agent continue/cancel | tool-pre rewrite/deny; tool-post annotate/replace | `unavailable` pending #55/#57/#59 |

This table reports current truth, not the roadmap target. A downstream lane may
change a state only after implementing and testing the frozen semantics.

## Completion rule

The parity claim becomes green only after #60 proves every `contracted` row by
implementation or changes it to a reviewed explicit exclusion, validates each
provider capability cell, and closes #54–#59. Until then this document is a
contract and progress manifest, not a completed parity claim.
