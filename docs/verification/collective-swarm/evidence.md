# Collective swarm implementation evidence

Implementation owner: current worktree only. Baseline research commit `1ca48b3`.
The existing contract and research were read before implementation. Current user
handoff overrides publication: no push/PR/merge; primary assistant publishes.

## Mode checkpoint

Implemented global/ask-local parsing, slash selection, persistent mode/cap,
checkpoint fields, collective policy, shared admission injection and isolated
subagent refusal. Existing task and system instructions remain composed.
Native release `make -j4` exited 0 on the initial mode working tree (2026-09-19).
A following reviewer correction allows enabling mode in an existing idle session;
this correction still needs behavioral checks. Cap changes once enabled require a
new session so an existing immutable admission scope cannot be widened.

Read `/tmp/tny-collective-0qjnf5vp/review.txt`: addressed idle enablement;
publication collision/atomicity and subscribe-before-snapshot tracked for the
messaging checkpoint. Full functional verification is pending. Overall INCOMPLETE.

## Messaging and public behavior checkpoint

2026-09-19, implementation after c20d17d, checkpoint content recorded by the next
commit. All commands ran from the assigned worktree, native Darwin arm64, pinned
mise tools, synthetic localhost credentials only.

- `make -j4`: PASS exit 0 (build-formatted.log). Native Release 1,120,816 bytes
  before final policy wording; final footprint to be measured again.
- `make test-unit -j4`: PASS exit 0 (unit1.log), including strict swarm count and
  failed task/session reconcile cap rollback regression.
- `python3 tests/integration/test_team_mailbox.py`: PASS exit 0, 31 cases
  (mailbox-final.log): original 25 plus publication snapshot/retry/ack,
  transaction-wide backpressure/collisions, queued/timeout/cancel/terminal/stale,
  quiet single snapshot, notification race and replacement.
- `python3 tests/integration/test_collective_swarm.py`: PASS exit 0, 11 cases,
  7.077s (collective3.log): both public tool profiles exchange a proposal,
  counterexample and reply before convergence; n=1; launch bypass refusals;
  global/ask-local mode and count syntax; task/system preservation; save/resume;
  stable system text; native PTY idle enable/rebind plus /new cap change;
  terminal publication fails nonzero. Two-peer flow uses exactly 4 lead, 5 proposer
  and 6 challenger requests, not model inbox polling.
- `python3 tests/integration/test_collective_cap.py`: PASS exit 0, 1 case,
  1.765s (cap2.log): two separately started real runs share cap1; second queues
  until first exits. Seven parent calls, one per worker. Also public CLI empty,
  deadline and SIGINT cancellation (exit130) without extra model calls.

Earlier failures retained outside repo in the handoff directory: mailbox-new.log
(quiet lock-creation hint and lock-contention race), collective1.log (reply BUSY),
collective2.log (PTY assertion expected an untruncated mock echo). Fixes: bounded
transaction-lock retry, initial quiet fixture normalization and correct PTY oracle.
These failed runs are not represented as passes.

Independent read-only Codex review reported failed-resume cap rollback, /new cap
change refusal and omitted timeout schema. All corrected; rollback has a unit
regression, /new has PTY coverage, schema regression is being added. Coordinator
also reproduced idle writer handoff race, fixed by graceful end/reap followed by
locked reload before binding/saving mode. Additional reviewed fixes: bounded
watch draining; opt-in-only collaborator policy; safe adoption of old owned work;
legacy retry refusal; permission detail includes action; only wait treats terminal
as observational success; publication receipts omit N copies of the payload.

Current delivery: atomic publish over existing records, per-recipient replay and
ack; kernel-notified bounded wait with separate outcomes; runtime shared admission
across launches; durable opt-in collective policy. Full gates, Linux/wasm execution,
mutations and remaining focused regressions are still pending. No cache-hit or
real-model convergence/quality claim. Primary assistant will own full `make test`
and frozen wasm checks per coordination file; this process owns quality/leaks and
focused followups. Overall verification gate remains INCOMPLETE.

## Followup checkpoint and final focused results

Post-a97f330 product-source corrections are limited to:
1. `team_control.c`: scope one-worker compatibility to captured swarm leads;
   ordinary operator/team validation remains unchanged.
2. CLI help: advertise ask --swarm and mailbox --timeout-ms/publish/wait, remove
   duplicated root entry.
3. `cli_swarm_preflight` before workspace/SSH setup: ask-local mode refuses SSH,
   ephemeral/unsupported contexts before connecting or creating a worktree.

Current focused results (working-tree input manifest in artifacts/input-manifest.json):

| Command | Exit/status | Evidence |
| --- | --- | --- |
| `make -j4` | 0 PASS | build-help.log |
| `make test-help-flags` | 0 PASS, 4 tests | help-final.log |
| `make test-unit -j4` | 0 PASS, before final CLI preflight addition | unit3.log; final rerun follows |
| `python3 tests/integration/test_collective_swarm.py` | 0 PASS, 13 tests, 10.302s | collective-final5.log |
| `python3 tests/integration/test_collective_cap.py` | 0 PASS, 1 test, 1.663s | cap-final.log |
| `python3 tests/integration/test_team_control.py` | 0 PASS, 7 tests, 27.360s | team-control-final.log |
| `python3 tests/integration/test_swarm_delivery.py` | 0 PASS, 5 native cases; 1 wasm case skipped | delivery.log |
| `python3 tests/mutation/collective_mailbox.py` | 0 PASS, baseline/restored 33 mailbox cases; 3/3 behavioral kills | artifacts/mutations.json; mutations2.log |
| `make leaks` | 0 PASS, native leaks clean at a97f330 | leaks1.log |
| `make quality -j4` | 2 FAIL: pre-existing Python formatting only after local lint fixes | quality-final.log |

Logs named above are under `/tmp/tny-collective-0qjnf5vp/`; durable mutation detail
and the input manifest are committed here. Mutation1 initially survived because
payload length also changed; a same-length conflicting body now kills removal of
content comparison. Other killed guards: exact outstanding capacity and replacing
notification-driven wait with periodic resnapshot. No mutant touched live sources.

The 13 public cases now additionally prove SSH is never invoked on ask-local
unsupported mode, old live parent work prevents adoption, non-swarm teams keep
their ordinary prefix, old jobs cannot be retried around admission, maximum-sized
publication bodies return compact parseable receipts, and restored/finalized run
observations use actual terminal state. Unit additions cover timeout schema and
checkpoint cap/explicit selection. A new unit compile initially failed on intentional
adjacent string literals; parenthesized and passed unit3. A first terminal assertion
ran before run finalization (item completion is earlier); fixed to observe terminal
run state. Old-work refusal test now counts lead requests rather than racing an
already-authorized second legacy worker. Failed logs remain visible.

Quality's remaining failure is unchanged `tests/integration/test_worktree.py:460`
(Ruff wants its long assert split). `git show 1ca48b3:tests/integration/test_worktree.py`
piped to `ruff format --check --stdin-filename tests/integration/test_worktree.py -`
also exits 1. It is unrelated and unchanged. No blanket exclusion or weakened gate.
Final source-specific lint/static checks and unit rerun follow this checkpoint.

Coordinator evidence inspected: frozen a97f330 full `make -j4 test` exited 2 at
help alignment before integration; the corrected help gate now passes. Frozen
Emscripten 6.0.8 Node/browser build plus Node mode/mailbox refusal exited 0, zero
HTTP and zero jobs. Browser runtime and Linux inotify remain unverified. Primary
assistant owns a new frozen full-suite/platform reconciliation, not this process.

Toolchain observed: Darwin arm64, Apple clang 21.0.0 (clang-2100.3.27.1),
clang-format 23.1.0, Ruff 0.16.6. Stripped native release: 1,120,816 bytes;
`otool -L` lists libc++.1.dylib and libSystem.B.dylib only. This is measurement,
not a before/after performance claim. Only fresh ADR 0156 differs from research
baseline; pre-existing ADRs are unchanged (pre-existing 0153/0154 duplicate numbers
are not modified). No live provider inference, push, PR, merge or release.

## Coordinator reconciliation at source revision 25993c6

This section supersedes earlier checkpoint status. Implementation is delivered;
full-gate success must not be inferred from the focused results.

`25993c6` is the final production-source checkpoint. Subsequent delivery/evidence
edits are documentation only. The new test entrypoints accept the full integration
runner's executable argument (`8efc60c`). An existing unrelated assertion needed
only Ruff formatting (`10d33f7`); no gate was weakened. Two publication comparisons
now spell out `!= 0`, preserving semantics and passing static checks. The mutation
artifact matches current mailbox source SHA256
`a8d6e667cb056950b659b6873f07e4202b18d578a150f40778585b9b1f4d194d`.
Baseline/restored mutation runs passed; all three injected regressions were killed.

Fresh final native unit execution: 512 passed, zero failed/skipped, 29,270
assertions. Additional isolated schema checks passed. Full regression, quality,
leaks, focused integration and wasm gates are being collected on separate frozen
worktrees of 25993c6; their final exits will be appended below.

The prior e47c9cd full attempt reproduced a **baseline** background dashboard
failure (`locked_saved_inspection_retry`, waiting for `Saved read-only`). The
identical failure exists in the 3751ef9 baseline log. It also exposed the two new
runner-entrypoint defects, now fixed and independently rerun: cap 1 case passed;
collective 13 cases passed. That superseded full attempt was explicitly stopped
(exit -15) after capturing the failures, and no process from its worktree remained.
The first a97f330 full attempt exited 2 at help alignment, corrected in e47c9cd.
The baseline aggregate attempt hit its 1,800-second safety deadline; it is not a
complete baseline pass. No earlier failed or interrupted run is relabelled green.

See [independent review](review.md) for findings and their resolutions, and
[Linux verification](linux-verification.md) for the clean Linux Clang build,
public peer/cap/delivery successes and the independently reproduced gVisor
filesystem limitation. The disposable Linux sandbox has been terminated.

Actual provider cache hits, model-quality improvements, performance speedups,
browser runtime and Windows execution were not measured. Stable-prefix and
bounded-context fixture evidence establishes cache capability, not cache billing.
