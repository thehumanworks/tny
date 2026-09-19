# Independent review and resolution

An independent read-only Codex review completed with exit 0. It inspected actual
source, did not modify the worktree, and distinguished source-derived findings
from runtime evidence. Its three actionable findings were resolved:

| Finding | Resolution and evidence |
| --- | --- |
| Failed resume could clear the original session's swarm cap before task reconciliation failed | Restore prior cap on failed reconciliation; unit regression plus an independent real `TNY_ISOLATE=0` PTY test preserving cap 2, task, history and failed target file |
| `/new` retained the old cap and prevented the documented fresh selection | Permit fresh-session cap selection; public PTY test covers `/swarm 2`, `/new`, then a different cap |
| Mailbox wait schema omitted required `timeout_ms` | Add the property and a schema regression; public wait fixtures exercise deadlines and zero-timeout observation |

The coordinating assistant independently reproduced the idle writer handoff race
when `/swarm` was activated after an ordinary turn. The implementation now releases
and reaps the old writer, reloads under ownership, persists mode, and rebinds.
Five independent PTY repetitions passed with two actual provider requests,
preserved history/cap and clean process exit.

Further direct review hardened bounded event draining, compact publication
receipts, terminal send/publish failure status, opt-in-only collaborator policy,
legacy retry/adoption limits, permission detail and ordinary-team compatibility.
The full integration runner then exposed an argument-handling defect in the two
new test entrypoints. Commit `8efc60c` fixes that through the existing argument
helper; both exact runner-style invocations passed after correction.

An earlier Claude Fable/high review reached its 600-second deadline without a
report. It is not counted as a successful independent review or approval.
The implementation agent likewise reached its owned execution deadline after
substantive commits. The coordinating assistant inspected and committed its final
explicit-comparison/static-check changes and owns final verification/publication.

Detailed private command logs remain in `/tmp/tny-collective-0qjnf5vp/` on the Mac.
No coding-agent internal reasoning logs are published with this change.
