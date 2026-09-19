# Agents session inspection and continuation

Date: 2026-09-19
Baseline: f90a6103b3b9d3e982bc88a8edd12fd0b69ddaf3
Status: implemented, independently reviewed, and native acceptance verified.
See [the evidence record](agents-session-continuation-evidence.md) for gate results,
environment retries and untested platform limits.

## User-visible goal

A background session's terminal turn status is not the end of its conversation.
`tny agents` must let users open saved conversations even when owner attachment
fails, and let them explicitly continue the same session when ownership is
available. A live runner may retain its writer lock after persisting `done`.
Neither stored status nor a failed connection grants permission to steal it.

Preserve successful live-owner attachment on Enter. When attachment cannot be
obtained, show a clearly labeled saved read-only transcript instead of leaving
the user at the generic `cannot reattach` error. Completed/unlocked rows open
without starting a provider. A prompt submission or documented explicit control
action may acquire ownership and continue. No forced takeover is introduced.

## Verification contract

| ID | Required behavior | Required evidence |
| --- | --- | --- |
| A1 | Completed sessions remain readable and continuable after the old runner actually exits, retaining session ID and history. | PTY regression using a real loopback-backed completed session; observe writer release; reopen in a fresh dashboard; submit follow-up; assert same ID, preserved history and exactly one additional turn/request. |
| A2 | A held writer lock, unreachable runner, or competing live owner cannot prevent saved transcript inspection. Label the fallback read-only and explain how to continue. | Deterministic held-lock/unreachable fixture and competing-owner PTY case; saved output visible; no generic attachment-only dead end. |
| A3 | Inspection of completed/fallback rows performs no provider inference or credential refresh, starts no runner, activates no saved checkpoint and does not save the session/settings/auth stores. Unavailable provider configuration does not hide saved text. | Count loopback requests, compare persisted bytes/artifacts, inspect without provider credentials/config; checkpoint inspection regression. Existing live attachment may consume the existing stream, never repost the turn. |
| A4 | Explicit continuation safely attaches to an available live owner slot or acquires the writer and reloads the saved session before a new runner executes. A competing owner/held lock rejects execution without a new writer, socket replacement, persistence, or lost history. A failed probe/handshake may fall back to inspection, never authority. | Lock/contention tests; competing-viewer continuation case; latest-under-lock transcript check or existing writer-lock regression plus focused race case. Do not remove ADR0104's teardown barrier. |
| A5 | A fallback view cannot mutate a live writer's replica through `/new`, `/rename`, `/compact`, settings/provider switches or exit. Failed continuation remains retryable after ownership becomes available. | PTY mutation guard and exit checks while a synthetic writer lock is held, followed by successful continuation after release. |
| A6 | Existing live reattachment preserves the active turn, permission mode, pending decision, model and workspace. New execution uses the selected session's provider/model/workspace and safe existing resume configuration rules. Do not claim to reconstruct configuration never stored by legacy sessions. | Existing permission/multitool/restart/reattach and worktree suites; test new continuation model/provider selection. Document legacy/current-settings limitation instead of silently asserting exact recovery. |
| A7 | Checkpoint inspection is distinct from execution. Recovery remains validated and exactly-once; explicit recovery may start retained work, but opening an unowned saved row alone may not. | Existing recovery checks plus new no-activation inspection coverage. Document the explicit recovery action and its prompt/queue semantics. |
| A8 | Dashboard/view exit continues to detach background work; ordinary foreground quit/interrupt behavior stays unchanged. Unsupported wasm/in-process paths fail safely before mutation, not via an unlocked fallback writer. | Background, interruption, isolation and TUI regression suites; focused in-process continuation rejection if applicable; native/wasm behavior documented. |

## Implementation constraints and scope

- Keep C11 application code and existing scoped private C++20 runner ownership.
- Preserve single-writer locking, fresh-under-lock reload, role authorization,
  finalization ordering, listener ownership and existing public ABI.
- Resolve provider credentials only when execution requires them, not to render
  a saved transcript. Do not serialize credentials or invent a new daemon.
- Keep the successful live-attach path usable; do not turn every active session
  into a disconnected viewer or force users to repost an active turn.
- Separate read-only authority from the existing background detach-on-exit bit.
- Prefer the existing session store and runner protocol. Full historical
  configuration persistence, force-takeover, and new provider behavior are out
  of scope. Document unavailable legacy settings honestly.
- Update CLI/TUI/session documentation, help text and a new ADR for changed
  inspection/checkpoint activation semantics. Extend existing tests where
  practical; new fixtures/targets require matching Nix updates.
- Synthetic credentials, loopback mocks and temporary HOME/XDG only. No live
  inference or inspection/modification of the user's private sessions.

## Independent work and acceptance

1. Read-only investigation by subagent 236723587a413e94.
2. Implementation and regression tests by a separate implementation subagent.
3. A different, read-only reviewer checks the actual diff against A1–A8; findings
   are fixed and reviewed before acceptance.
4. Parent reruns focused checks and required gates. Worker/reviewer claims alone
   are not acceptance evidence.

## Exact QA commands

Run in a clean temporary HOME/XDG environment with inherited provider secrets
removed and `TNY` set to the absolute built release binary. Preserve access to
installed build tools through PATH. Test fixtures must isolate their own homes.

```sh
make release debug
./build/tny-test -s runner_suite
./build/tny-test -s session_bg_suite
./build/tny-test -s tasks_suite
python3 tests/integration/test_background_agents.py "$TNY"
python3 tests/integration/test_isolation.py "$TNY"
python3 tests/integration/test_background.py "$TNY"
python3 tests/integration/test_interrupt.py "$TNY"
python3 tests/integration/test_worktree.py "$TNY"
python3 tests/integration/test_tui.py "$TNY"
make test
make quality
make leaks
wc -c build/tny
ldd build/tny
```

If runner resource ownership changes, also run `make test-runner-ownership`.
Build flags and the exact test revision/input digest belong in the evidence.
Performance improvements are not claimed. Do not commit generated binaries.

## Evidence

See [the completed evidence record](agents-session-continuation-evidence.md).
The parent observed successful release/debug builds, the 497-test sanitizer unit
suite, all 76 integration entrypoints (including 25 background-agent cases), the
full CI-aligned quality gate, the complete Nix-backed Valgrind gate, and four
contract-negative controls. Expected optional/platform skips remain explicit.

Independent review found one inherited-provider-wizard guard bypass; the
implementer fixed it, added unit/PTY regressions, and the reviewer accepted the
revision after rerunning those checks. No implementation blocker remains.
macOS/wasm execution and a full Nix flake check were not run. Legacy configuration
limits, environment/toolchain retries, input hashes, artifact size and runtime
dependencies are preserved in the evidence record.
