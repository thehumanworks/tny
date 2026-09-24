# 0176 — Search explicit paths and preserve subagent display labels

Date: 2026-09-24. Status: proposed.

## Decision

File search honors a caller-named file or directory even when a workspace-root
walk ignores that directory. A named workspace root or ordinary directory
keeps normal ignores and the normal file budget. Directory walks always skip
hidden entries below the walk root, including `.tny` stores inside a workspace.
The root's own ancestors do not exclude a workspace under `.tny/worktrees`.
An explicitly named `.tny` store still hides auth, token and secret files; a
named regular file is searched directly. A named generated directory outside
the workspace also searches its ignored descendants. `grep_files` returns
literal matches before regex-only matches, so broad regex hits cannot crowd
literal code matches out of the 500-hit cap. A regex that matches the empty
string or contains unsupported letter escapes is disabled. `glob_files`
interprets `**/` as zero or more directories, expands comma-separated braces,
and accepts patterns rooted at an explicitly supplied path. Both tools explain
an empty result with scan and ignore counts. The same C code runs on native
and wasm.

A supplied `id` on `subagent` create is an optional display label. The generated
16-character lowercase hex session id remains authoritative. A durable child
stores its parent session id and label beside its session as soon as the child
id is known, including when its first turn fails. Follow-up actions accept the
label only when it resolves to exactly one child of the current parent session.
Duplicate labels for one parent are rejected before create; different parents
may reuse a label. A sessionless caller uses an empty parent scope. An
ambiguous lookup fails closed.
The label file shares the session's lifetime, and an ephemeral child gets no
resumable label. This revises
`0087-explicit-subagent-contract-and-private-launch.md`'s rejection of a create id to address
observed model calls while preserving its session-id and process ownership
rules. No credential or provider configuration enters the label file.

## Evidence and rollback

The local replay script in `tests/bench/replay_tool_search.py` extracts the
recorded `grep_files` and `glob_files` argument shapes from saved sessions and
replays them against synthetic local fixture trees. Grep fixtures preserve the
pattern's exact special characters as literal text. Its before/after no-match
counts are reported in the worker status file; this is an offline diagnostic,
not a live inference or task-success claim. The optional experiment flag is
`none (bug fix)`. The hand-written tool-schema JSON grows from 25,309 to
25,566 bytes (+257); this increases the request's tool-schema prefix on each
turn, while leaving the reasoning items unchanged.

The 197 recorded shapes (170 grep, 27 glob) produced 34 and 16 no-matches
respectively with the baseline, and 0 and 0 with the changed implementation.
The replay's nine outside-workspace `node_modules` grep calls go from nine
no-matches to zero; its three brace globs go from three to zero. Among 18
calls whose workspace sits under `.tny/worktrees`, ten baseline no-matches
become zero.
The replay keeps outside-workspace paths outside its synthetic workspace,
places recorded `.tny/worktrees` workspaces beneath `.tny`, seeds one exact
literal match for each recorded grep shape, and seeds a real file from each
brace glob. It measures path, ignore and glob behavior under that fixture
construction. It does not measure original project truth or the benefit of
regex intent; dedicated unit fixtures verify alternation and literal code
syntax.

Rollback is a revert of this change. Existing generated child ids remain valid
throughout. Removing the label lookup leaves stored sessions addressable by
generated id; `subagent-label` files can remain beside them harmlessly.
