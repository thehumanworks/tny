# ADR 0048: Runtime task presets (issue #81)

- Status: accepted
- Date: 2026-08-29

Task presets are runtime configuration shared by CLI, TUI, workflows and
language SDKs. The public selector is `--task`; `--agent` remains reserved for
ACP executable/endpoint selection.

Built-ins are `review`, `optimizer`, `document`, and `retro`. CLI discovery is
lazy and deterministic: an explicit workflow `TNY_WORKFLOW_TASK_DIR` entry, then the
project `<workspace>/.tny/tasks/NAME.md`, then `~/.tny/tasks/NAME.md`, then a
built-in. Project files never grant authority or override explicit flags.
Markdown bodies are UTF-8, NUL-free regular files no larger than 256 KiB; task
names match `[A-Za-z0-9_.-]{1,63}`, cannot begin with `.`, and cannot contain
`..`. Symlinks and other non-regular files are rejected. Optional frontmatter
accepts only `name` and `description`; it is stripped before composition. Discovery is
bounded at 256 definitions per context. SSH sessions do
not inspect the local launch tree and currently accept built-ins only, avoiding
wrong-project reads. The TUI refuses `/ssh` while a non-builtin task is selected;
the user must clear it before attaching and may then select a builtin.

Composition is tny-owned runtime/safety instructions, normal project and user
context (including AGENTS.md and the skill catalog), task instructions, then
explicit system-prompt additions, followed by the user prompt. Host providers
whose pinned protocol has no system field receive the task and explicit-system
sections in that same relative order on the first fresh turn; resumed or already
started sessions reject mid-session task changes. A fresh session persists the
resolved name, stable source category, digest, and exact body snapshot (the
body lives in a private sidecar and is never emitted by session/status JSON).
Every resume spelling restores that snapshot when no task was explicitly
requested. An explicit selector is accepted only when its name and digest
match; a session with turns but no saved task rejects grafting one later. TUI
`/task` lists/selects or clears a session-scoped preset and requires `/new`
before changing it.

Deterministic embedders never perform ambient discovery. ABI 1.1 adds the sized
`tny_task_options_v1` and `tny_runtime_options_v2` records plus corresponding
initializers/create symbol in the `LIBTNY_1.1` node while every pre-existing
symbol remains in `LIBTNY_1.0`. A built-in is selected by name with an empty body;
custom SDK tasks provide an explicit body. The existing capability masks gain
`TNY_CAP_FEATURE_TASK_PRESETS`, available in ABI 1.1 and enabled only for a
runtime with a selected preset. Python names the runtime field `task_preset`;
TypeScript names it `taskPreset`. Name/source/digest may appear in status
metadata, but bodies and credentials never do.
