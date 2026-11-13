# 0221 — Remove features without evidence of value

Date: 2026-11-12
Status: accepted

## Context

The owner's goal is a small, old-school coding agent judged on feature set and
simplicity. A read-only audit on `eebc162` ranked code by cost (complexity,
fragility, binary size) against evidence of value found in the repository:
callers, user paths, tests against real models, and the project's own
verification reports. The owner then delegated the calls to the maintainer.

## Decision

Remove each feature below. Each removal is one commit, so it can be reverted on
its own. A removed feature can return through a new ADR that brings evidence.

### Jev decisions (`tny score`, `tny choose`)

Supersedes [ADR 0165](0165-tnyjev-decision-engine.md). The client needed a
separate paid TypeSafe key, had no caller inside tny, and was only tested with
loopback fixtures. Removed: `src/core/tnyjev*`, `src/cli/cmd_jev.c`, their tests
and docs (about 1.5k lines including tests). Open issues #180 to #187, which
would route reasoning and models through Jev, are out of scope.

### Default automatic workflow learning

Supersedes [ADR 0201](0201-default-automatic-workflow-learning.md). The learner
was on by default and taught one rule: read a file before retrying a failed
exact edit. Its evidence used a deterministic fixture model, and its own report
said it does not show that a real model follows the advice. It added state in
`~/.tny/learning/`, changed request prompts, and threaded a fact through the
backend, tools, execution-server state and job snapshots. Removed: the learner,
its store, the `--no-self-improve` flag, `TNY_SELF_IMPROVE` and the
`self_improve` setting (old settings files keep loading; the key is ignored).
`status --json` and `doctor --json` no longer report `self_improve`. The optional
instruction-experiment workflow (ADR 0153) stays.

### libtny ABI 0 compatibility build

Amends [ADR 0037](0037-libtny-abi-1.md). `make lib-shared` checked out commit
`510a95c` and ran a nested build of it to produce `libtny.so.0` /
`libtny.0.dylib`, plus a separate header root and `libtny-0.pc`. No outside ABI
0 consumer is known; the SDKs load only ABI 1 and reject ABI 0. Removed: the
compat build, its manifest (`abi/compat0.json`), consumer tests and the
`libtny0-compat` release tarball. This ends the published ABI 0 maintenance
window early. Earlier release assets stay available; revert this commit to
restore the window.

### Formal checks outside `make test`

Amends [ADR 0171](0171-formal-verification-smt-lib.md). `make test` ran
`verify-formal`, which fails without Clang and Z3, so a plain checkout could not
run the test suite. `verify-formal` stays a target; CI's full suite and the Nix
check run it as a separate step.

### Dictation and speech (`tny dictate`, `tny speak`, `speak` tool)

Supersedes [ADR 0070](0070-provider-independent-speech.md),
[ADR 0071](0071-ephemeral-host-audio-playback.md),
[ADR 0079](0079-provider-independent-dictation.md),
[ADR 0175](0175-dictation-transcript-normalization.md) and
[ADR 0176](0176-dictation-normalization-default.md). A coding agent does not
need a microphone, a speaker or an STT subscription to do its task. The two
features added two provider adapters (Codex and xAI STT), host recorder and
player processes, a transcript normalizer with a Lean proof package, a
`dictionary.json` schema, a TUI recording mode (Ctrl-R, `/dictate`), a fixture
binary build, three standalone toolkit operations and the global
`--xai-api-key` flag. Their tests ran only against fixtures, and 22 of the
baseline integration tests for xAI dictation already failed on this host.
Removed: `src/core/dictation*`, `src/core/speech.*`, `src/util/audio*`,
`src/cli/cmd_dictate.c`, `src/cli/cmd_speak.c`, `src/tui/tui_dictation.c`, the
`speak` tool and terminal intercept, the toolkit operations `speak`,
`transcribe` and `dictate` with their SDK methods, `tests/formal/dictation`
and the related tests and docs (about 3.6k lines of `src/`, 7.3k lines of
tests and SDK code). The prompt text check that optimisation shared with
dictation moves to `tny_prompt_text_valid` in `core/optimise.c`. The settings
key `dictation` stays reserved, so old settings files still load.

### Swarms, team control and the team mailbox

Supersedes [ADR 0146](0146-durable-team-mailbox.md),
[ADR 0148](0148-team-control-over-jobs.md),
[ADR 0156](0156-collective-swarm-mode.md),
[ADR 0157](0157-purposeful-file-defined-swarms.md),
[ADR 0158](0158-purposeful-capability-surface.md),
[ADR 0160](0160-swarm-contribution-contracts.md),
[ADR 0161](0161-typed-swarm-messages.md) and
[ADR 0162](0162-durable-swarm-review-continuity.md). The project's own live
evaluation of swarm contribution contracts found "no acceptance or speed
improvement" over a single agent (3 of 3 tasks accepted in both conditions,
the swarm slower on every task and with more tool errors), and its purposeful
swarm report advised preferring a single agent for small work. The feature set
was large and reached into the core: a manifest compiler, a mailbox service,
team control and a team tool, typed swarm messages, three CLI commands
(`tny swarm`, `tny team`, `tny mailbox`), the `--swarm` and `--swarm-file`
flags, `/swarm`, member capability bearers and dependency-evidence prompts in
`jobs.cpp`, swarm state in the session, checkpoint and execution-server
projections, and an ACP coordination-context queue that only swarm activation
and team messages filled. The owner chose to remove it.

Removed: `src/core/swarm*`, `src/core/team_*`, `src/core/tools_team.*`,
`src/cli/cmd_{swarm,team,mailbox}.c`, the `team_control`, `team_mailbox` and
`swarm_message` tools and terminal intercepts, the ACP pending-context queue,
`examples/swarm`, `schemas/swarm.schema.json`, the swarm benchmarks and their
tests and docs (about 7.1k lines of `src/` and 10.8k lines of tests).

Kept: durable jobs with DAGs, workspace policies (including
`shared_read_only` and `TNY_TEAM_READ_ONLY`), shared admission, managed task
workspaces and synchronous subagents. Job records keep the four-field DAG
definition hash, so existing non-swarm records still verify. Jobs no longer
issue member capabilities, write `mailbox_capability_sha256`, accept
`peer_messages`, or report `startup_error_code`. Public checkpoints written
before this change still recover: their swarm fields are ignored and the
identity hash keeps an empty slot where the swarm digest was. A checkpoint of a
session that had an active swarm no longer matches and is refused.

## Kept after review

### Code mode and the embedded CPython

The audit ranked the embedded CPython first by size: it is about 25.9 MB of the
27.3 MB stripped Linux x86-64 release binary. Before removing it, a live A/B ran
the 12 `tests/bench/harness_bench` tasks once each with the same build in two
modes: the current `run_code` code mode, and a temporary build that advertised
the native tools directly. Model: OpenCode `beam-free`, chat wire, 2026-11-12.

| Mode | Tasks passed | Input tokens | Uncached input tokens | Requests |
| --- | --- | --- | --- | --- |
| Native tools direct | 11 / 12 | 1,696,923 | 281,947 | 177 |
| `run_code` code mode | 11 / 12 | 1,318,932 | 203,412 | 174 |

Token and request totals cover the 11 tasks where both runs finished; the
code-mode `build-fix` run hit the 600 s timeout, although its verifier still
passed. Each mode failed one different task (`codebase-qa` direct,
`rename-refactor` code). With equal results, code mode used about 22% fewer
input tokens and 28% fewer uncached input tokens, so it stays the default and
the embedded interpreter stays. Binary size alone is not a reason to remove it
(ADR 0150). The direct-mode build also showed that two tool schemas are not
provider-strict: `image_preview` has a top-level `oneOf`, and the
`sources` arrays of `image_export` and `image_contact_sheet` have no `items`.
They only matter if native tools are ever advertised directly again.

### Terminal intercept

Terminal tool profiles reach `tny edit`, `tny web` and `tny image` through the
intercept, so it is not dead code.

## Measurements

Linux x86-64, GCC, `make release`, stripped:

| | Before (eebc162) | After |
| --- | --- | --- |
| Release binary | 27,405,120 B | 27,192,128 B |
| Release binary without CPython (`SRC_PY_NATIVE=src/core/code_python_unsupported.c`) | 1,449,712 B | 1,236,720 B |
| `src/` C/C++ lines | 90,695 | 78,005 |
| Unit tests (all pass, 2 skipped) | 721 | 650 |
