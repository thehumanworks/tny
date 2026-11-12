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
