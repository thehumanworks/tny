# ADR 0112: Bundled task-creation preset

- Status at creation: accepted
- Date: 2026-09-14
- Requirements: R1-R3 in [the verification contract](../verification/task-creation/contract.md)
- Extends: [ADR 0048](0048-runtime-task-presets.md)

## Context

Users can create reusable tny task presets as Markdown, but the builtin catalog
only covers review, optimization, documentation and retrospectives. An agent
asked to author a task needs the actual file format, discovery rules and a
validation procedure without relying on a checkout of tny's documentation.

## Decision

Add `task-creation` to the existing C builtin registry. Its instructions explain
how to turn a user request into reusable task instructions, default to a project
`.tny/tasks/NAME.md`, use the user directory only when requested, inspect existing
definitions, preserve unrelated files and avoid unrequested replacement or
shadowing. It includes a parser-valid example and the exact supported name,
frontmatter, encoding and size constraints.

The agent uses its existing file or shell tools, subject to their normal
permissions. It reads back the file and, when available, checks the resolved
name, source and body with `tny task show` and validity with `tny tasks`. It gives
the user a concrete invocation. Authoring does not by itself request execution
of the new task, and changing to it requires a fresh session under ADR 0048.
These are model instructions, not new parser or filesystem enforcement.

There is no new tool, command, automatic task-selection heuristic, dependency,
provider setting or authority grant. Discovery precedence, project trust,
session snapshots and host/native prompt composition remain as defined by
ADR 0048. The builtin travels through those shared paths, including deterministic
SDK builtin selection. Browser/wasm uses the same instructions but only has
MEMFS files; missing filesystem or CLI access must be reported as unverified
steps with the proposed Markdown. SSH still discovers builtins only.

## Alternatives considered

- A standalone task file or skill would require an installation/discovery path
  and duplicate the already supported builtin mechanism.
- A new task-writing tool or CLI wizard could enforce a narrower authoring API,
  but adds code and a public surface for an operation existing tools and the
  task parser already support.
- Automatically selecting this preset from arbitrary conversation text would
  change task/session semantics and is unnecessary for the explicit workflow.

## Consequences and verification

The binary gains a small constant instruction body. Future format changes must
update the authoring instructions and example. Agent behavior remains dependent
on the chosen model; parser checks prove validity, not quality of arbitrary
model-authored instructions.

Unit tests load the builtin, parse its example and check project overrides.
The existing OpenAI integration suite creates and selects a task through real
tool execution on both native wire formats; its wasm CI invocation reuses the
same test. A live aiproxy/grok-4.6 run verifies actual model authoring and a
second turn using the created task. Results, review and ADR hash checks are
recorded in [the evidence](../verification/task-creation/evidence.md).
