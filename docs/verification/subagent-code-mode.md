# Subagents through code mode: evidence and learnings

Date: 2026-10-06. Baseline: `b0e13f65`.

## Reproduction

Live inference used the user's Codex ChatGPT subscription and `gpt-6-luna` at
low effort for both parent and child. Private HOME/workspace state and an
absolute `CODEX_HOME` kept test sessions separate without copying credentials.
The test explicitly selected `TNY_TOOLS=all`; the invoking shell's `terminal`
profile intentionally hides `subagent` and must not contaminate this fixture.

A child that only replied with text completed in about 1.8 seconds. A child
that ran a seven-second Python wait before writing a marker was cancelled by
the parent's omitted `run_code.timeout_ms`. After about 6.3 seconds, the parent
recorded `tool_ok:false` and `error: execution server timeout; outcome unknown,
not replayed`. The stored child exited 130 with zero completed turns, and its
completion marker was absent. The parent subsequently exited zero after
reporting the failure: its CLI exit alone did not establish delegated success.
The [sanitized baseline receipt](subagent-code-mode-before.json) identifies the
unchanged source revision and executable hash.

The new delayed-child mock fixture also fails against the unchanged preserved
baseline executable, with failed nested subagent and enclosing run_code records.

## Fix and acceptance

[ADR 0184](../adr/0184-code-cell-delegation-budget.md) defaults the whole cell to
the existing 600-second ceiling while preserving explicit shorter deadlines.
Schema and instructions derive the bounds from the runtime constants.

The fixture suite uses real production CLI, execution-server, Python and child
processes with synthetic loopback inference. The separate live checker uses
actual Codex inference and verifies schema discovery, child file work,
create/message across fresh cells, inspect/lifecycle, persisted completed turns
and returned marker results.
Its evidence stores allowlisted metadata, hashes, tool statuses and test markers;
credentials and provider payloads are not recorded.

```sh
make test-subagent-e2e
make verify-subagents-live
```

The [live receipt](subagent-code-mode-live.json) records Codex `gpt-6-luna` at
low effort. The usual `ask --json` probe completed with both parent and child
stored as `done`, exit zero, and a real 6.011-second child cell. A separate
canonical-event run completed schema discovery and four distinct successful
cells: create, message, inspect and lifecycle. The same child stored two
completed turns and exit zero. Its actual nested create/message durations were
11.087 and 10.400 seconds, measured from canonical events; both child markers
contained the expected nonce. All private sessions cleaned up successfully.

Canonical `--events=jsonl` currently uses an in-process parent
(`src/cli/cmd_ask.c`); its stored status/exit fields are unset. That probe checks
the real terminal event and CLI exit, without inventing a persisted `done`.
The separate usual-CLI probe verifies detached parent/child completion.
The checker permits only syntax-error retries proved to occur before nested
effects; this recorded live run needed none. These are functional observations
with deliberate delays, not inference performance measurements.

| Local gate | Observed result |
| --- | --- |
| `make -j6 test` | Exit 0; 631 unit cases (630 passed, one platform skip), 51,659 assertions, and all 103 integration groups passed |
| Delegation fixtures | Both HTTP wires: create and message each held beyond five seconds, real child effects, same-session resume, inspect/lifecycle |
| Cancellation fixtures | Omitted-budget SIGINT and explicit 1,500 ms deadline: interrupted/stale child, no surviving owned process tree, no late marker |
| `make -j6 quality` | Exit 0; final live-checker Ruff, format and syntax checks also passed |
| `make -j6 leaks` | Exit 0; zero leaks; macOS fork-suite exclusions remain explicit in the gate |
| Release footprint | Stripped executable: 25,195,808 bytes; dynamic dependencies: `libSystem.B.dylib` and `libc++.1.dylib` |

Native CI lanes now require the delegation fixtures; the full suite already
discovers them. Hosted Linux/macOS CI, Linux GCC analysis/Valgrind and hermetic
Nix results were not run in this local delivery. Unsupported wasm/embedding
behavior remains unchanged.

`make install` updated `~/.local/bin/tny`; its SHA-256 matches the release
executable used by the full suite. The final live checker ran that installed
path directly. Existing tny processes stayed active. Relaunch tny to load the
fix; select `TNY_TOOLS=all` for the typed subagent tool, because the current
shell's explicit `terminal` profile hides it.

## Learnings

- Exercise the advertised default. A fixture that always supplies a generous
  timeout can bypass the production failure even when it uses real processes.
- Discover the nested schema before calling it. Code-only provider requests do
  not expose `subagent.action`; guessing arguments can fail before launch.
- Test ordinary child work beyond the old deadline; a quick text response proves
  account/launch plumbing but does not establish usable delegation.
- Assert nested outcomes and child state, not just a successful parent CLI exit
  or the model's final claim. Require the real result marker and completed turns.
- A larger default requires explicit short-budget and interrupt regressions.
  Keep the five-second human-wait test explicit so it cannot pass vacuously.
- Separate synthetic-provider CI coverage from opt-in account inference, and
  record the tested executable hash so live evidence identifies its input.
