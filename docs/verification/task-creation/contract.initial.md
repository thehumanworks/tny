# Verification contract: bundled task creation

Created: 2026-09-14, before implementation.
Baseline: clean `fb232e8002b1ba18bfdf12ed723146e0ac1295ab` (`origin/main`).
Workspace: `/Users/tomas/projects/tny-task-creation`; branch `feat/bundled-task-creation`.
Outcome: select the bundled `task-creation` preset and ask a tny agent to author
reusable tny task presets. Existing task selection, discovery, validation, and
permission boundaries continue to apply.

The primary agent owns implementation and every invariant. A fresh subagent
performs one read-only code review, as explicitly requested by the user; that
single pass replaces the skill's default multiple checkpoints. Native goal
creation is not applicable: the active tool rule requires an explicit goal
request from the user or system/developer instructions, absent here. The
versioned contract records this ordinary implementation's requirements.

## Requirements and invariants

| ID | Requirement / family | Observable invariant | Checks |
| --- | --- | --- | --- |
| I1 | R1: bundled task creation | `task-creation` lists as valid/builtin and selects through the existing preset machinery without installation; its complete instructions reach the model. | C1, C2 |
| I2 | R1: author reusable tasks | Instructions explain the actual task format, default project scope, explicit user scope, supported metadata, validation, collision handling, and a usable invocation. Creation does not automatically execute the authored task. | C1, C2, C3, C6 |
| I3 | R2: working live workflow | `aiproxy` / `grok-4.6` creates a regular project preset in a disposable workspace; the real CLI resolves its body and a second live turn executes it with an observable result. Existing files remain intact. | C3 |
| I4 | Quality, regression, simplicity | Minimal C11 builtin addition, no new dependency/parser/tool or permission expansion; existing unit/integration, quality, leak, and native size gates pass (pre-existing failures separately established if encountered). | C1, C2, C4, C6 |
| I5 | R3: architectural decisions | New uniquely numbered ADR in the repository's `docs/adr/` convention describes the choice and boundaries; pre-existing ADR hashes are unchanged. | C5 |
| I6 | R4: independent single review | One independent subagent reviews the implementation and tests; valid findings are resolved and affected checks rerun. | C6 |
| I7 | R5: delivery | Task-owned changes committed and pushed to a remote feature branch; PR against main exists with matching head, correct description and honest check status. | C7 |
| I8 | Timing, complete scope, user understanding | Full initial contract preserved before implementation; every invariant reconciled against current inputs and evidence; handoff documents invocation and verification. | C0, C8 |

## Checks

All local commands run in the workspace above unless specified.

| ID | Procedure | Passing evidence |
| --- | --- | --- |
| C0 | Capture clean baseline, initial full contract and SHA-256 before implementation. | `contract.initial.md`, `contract.initial.sha256`, `adr-baseline.json`, initial evidence entry. |
| C1 | Extend `tests/test_tasks.c`; run `build/tny-test -s tasks`. | Builtin listing/selection, task example parses and round-trips; project override still wins. |
| C2 | Extend existing `tests/integration/test_openai.py`; run it with the built binary. | CLI listing/show and actual provider request contain the task instructions; generated task fixture can be selected. Shared fixture also runs in existing wasm CI. |
| C3 | Live disposable-workspace creation and execution with `--provider aiproxy --model grok-4.6 --effort xhigh`. | Successful sessions, provider/model metadata, generated file, real list/show validation, result from running the authored preset, unchanged unrelated fixture. Secrets remain runtime-only. |
| C4 | `make test`, `make quality`, `make leaks`, `make size-check`; inspect `git diff --check`. | Exit status and useful summaries for current state. No performance improvement claim; size measured on stripped macOS binary. Hosted platform checks reported separately. |
| C5 | Allocate ADR serial as sole writer; compare all pre-existing hashes and check new serial uniqueness. | New ADR and integrity record. |
| C6 | One fresh read-only reviewer inspects code, docs and tests for format/workflow correctness and maintainability. | Reviewer identity, inspected state, findings and resolutions. |
| C7 | Push feature branch, create/read PR and compare remote ref to local HEAD. | Commit, remote SHA, PR URL/head/base/state. |
| C8 | Hash changed source/test/docs plus base revision and retained config/dependency inputs; reconcile R1-R5 and I1-I8. | Final evidence table and concise handoff. |

## Critical cases and mutation applicability

The implementation adds prompt data to the existing builtin registry and does
not change parser, resolver, persistence, or permission decision logic. Generic
operator mutation of that unchanged logic is not required. Challenge registry
coverage by temporarily renaming the builtin in an isolated copy: the new
builtin test must fail. Existing parser tests cover malformed frontmatter,
name mismatch, invalid names/UTF-8, symlinks, size limits and resolution precedence.
The task body must explain that it provides instructions, not extra authority.
A small fenced example in the builtin is validated through the real parser.

Native macOS live behavior is the direct acceptance target; the preset remains
shared portable C data. Browser/wasm has only MEMFS persistence and may lack a
shell/CLI for validation; document this limit and do not claim host persistence
or new SSH custom-task discovery. No new make target, fixture directory, tool,
or dependency is planned, so the existing Nix filesets remain sufficient.

## Amendments

None.
