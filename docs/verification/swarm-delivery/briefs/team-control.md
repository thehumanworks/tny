Your #159 commit 3105192 is integrated; now deliver remaining public async control
and explicit verification for #153 in your isolated context worktree. First
cherry-pick e351f40 and 4c7e0d3 so jobs DAG/workspace helpers exist. Own ONLY NEW
src/core/team_control.c/.h, src/cli/cmd_team.c, tests/integration/test_team_control.py,
docs/team-control.md and new ADR0141. No jobs.cpp/h, tools/intercept/dispatch,
Makefile/Nix, workspace helper or SDK changes. Lead wires registrations.
No further delegation, push/PR/merge/issue closure. Read issue153 and contract.

Implement a THIN shared service over existing jobs, no second lifecycle authority.
job id=run, item index=task, item/job attempt=fence. `tny team start --request FILE|-`
(or documented simpler grammar) validates dag:true and one lead + >=2 workers
explicit spec, calls existing jobs submit, returns immediate handle. Equivalent
agent tool should use same parser/permission/detail/run API you provide. No
launch/collection success can mean accepted/tested work. start --template optional
only if real useful no scaffolds. Preserve existing jobs/synchronous subagent APIs.
Add status/collect/bounded wait-any/cancel. wait-any returns first terminal item
not already seen by caller's cursor/list, with timeout 124 meaning wait ended not
cancel. Do not poll inside model turns when safe notifications available (lead
is implementing notifications), but support client fallback. Collect uses bounded
job logs and authoritative session result, not unbounded concatenation. Cancel
must require expected_attempt and pass it through to core; existing core cancel
is being fixed by scheduler owner to enforce attempt fence. Reads/control on
wrong member must not accept arbitrary session ID. Trust runtime caller context
separate from args; ordinary local CLI operator has same-user job authority, tool
caller must be captured session/member. Preserve native/SSH/embedded/wasm refusals.

Explicit verification: accepted completion requires actual configured check
execution or remains unverified. Provide `team verify` with explicit caller-chosen
command (never worker prose), run/task/expected attempt and bounded timeout. Use
existing host process/shell seams, no raw poll/new provider loop; capture actual
exit, bounded output/hash, command, task result hash, exact workspace revision,
and timestamps. Persist checks to private sidecar under job directory; lock the
job owner nonblocking to fence retry; reject active/cleanup-unknown jobs and dirty
ambiguous revisions. A failure or uncertain cleanup remains failed/unverified,
never accepted. Permission identity team_verify must include exact command/cwd/
identity. Read/status grant cannot execute. Do NOT run verification automatically
because a worker says done. Record separate execution vs verification vs integration.
If full check-service implementation is unsafe within this slice, implement real
bounded async controls first and report verification gap rather than fake status.

Current scheduler extension in progress: DAG items have workspace policy and
prepared cwd; raw item.request.workspace describes requested policy, item.workspace
will carry resolved metadata. Authoritative root.workspace is launch cwd. Explicit
workspace integration is handled by separate service worker; don't duplicate Git
merge/cleanup. Read-only run inspection in `agents --run` is lead-owned.

Test public C/service boundaries with deterministic provider fixtures imported
from test_jobs.py; build temporary CLI driver if root dispatch isn't wired yet,
then return exact integration test requirements. Need overlapping worker handles,
responsive start, bounded wait-any/cursor/cancel unrelated run, and real success/
failure verification with provenance. Run focused tests/format/static analysis.
Commit and return SHAs, C interfaces, commands/exits, acceptance mapping and gaps.
