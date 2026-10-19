# Python extension examples

These examples use tny's typed, pure-stdlib `tny_ext` API. They are working
fixtures: `tests/extensions/test_examples.py` loads them through the same
persistent Python host used by tny.

> Extensions are trusted programs with your full user permissions. Read an
> extension before installing it.

## Inspect and check before installing

From the repository root:

```sh
PYTHONPATH=python python3 -m tny_ext catalog --json
PYTHONPATH=python python3 -m tny_ext check examples/extensions/verify_once.py --json
PYTHONPATH=python python3 -m tny_ext check examples/extensions/ci_guard --json
make test-extensions-python
```

`catalog` loads no extensions. `check` runs imports and setup for only the
explicit trusted paths, with full user permissions, but does not call their
event handlers or start inference itself. It is not a sandbox or static check.
For capability-dependent setup, add `--capabilities` with saved
`tny doctor --json` output. See the
[development guide](../../docs/extensions.md#develop-and-check-a-plugin-offline)
for diagnostics, exit statuses, and the reusable Python test API.

## Install one example

Direct-file extensions:

```sh
mkdir -p ~/.tny/extensions
cp examples/extensions/project_context.py ~/.tny/extensions/
```

Directory extensions:

```sh
mkdir -p ~/.tny/extensions
cp -R examples/extensions/ci_guard ~/.tny/extensions/
```

Restart tny after adding or changing extensions. `tny doctor` reports how
many entries were discovered. Use `--no-extensions` for a run without hooks.

## Included examples

| Example | Events | Behavior |
| --- | --- | --- |
| `log_events.py` | `*` | Appends provider-neutral event metadata to a JSONL file |
| `project_context.py` | `session_start`, `before_agent_start` | Adds visible context from `.tny-context.md` before the session's first provider turn |
| `stop_on_tool_failure.py` | `tool_end` | Requests agent cancellation after a failed tool |
| `verify_once.py` | `agent_end` | Requests one visible verification follow-up when a marker is missing |
| `ci_guard/` | `tool_end`, `agent_end` | Stateful package with a relative import; asks the agent to recover failed tools once |

`log_events.py` writes to `~/.tny/events.jsonl` by default. Override that with
`TNY_EVENT_LOG=/path/to/events.jsonl`.

`project_context.py` reads `.tny-context.md` from the workspace once per
session, immediately before its first provider turn. Override the path with
`TNY_CONTEXT_FILE=/path/to/context.md`.

Continuation hooks are unlimited by default. During development, a cap is
useful protection against a faulty condition:

```sh
tny --max-extension-iterations 2 ask "finish this task"
```

`tool_start`, `tool_progress`, and `tool_end` are normalized observations.
Use `pre_tool_use` for native pre-execution controls, and check
`api.capabilities` before requesting an action. Observation does not imply
that a provider exposes the corresponding control boundary.
