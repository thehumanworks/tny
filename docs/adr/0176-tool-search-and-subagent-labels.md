# 0176 — Search explicit paths and preserve subagent display labels

Date: 2026-09-24. Status: proposed.

## Decision

File search honors a caller-named file or directory even when a workspace-root
walk ignores that directory. The default walk still skips hidden and generated
directories. `grep_files` uses a substring fast path for plain text and POSIX
extended regular expressions for recognizable regex syntax. `glob_files`
interprets `**/` as zero or more directories and accepts patterns rooted at
an explicitly supplied path. Both tools explain an empty result with scan and
ignore counts. The same C code runs on native and wasm.

A supplied `id` on `subagent` create is an optional display label. The generated
16-character lowercase hex session id remains authoritative. A successful
durable child stores its label beside its session, and follow-up actions accept
that label only when it resolves to exactly one session in the workspace.
Duplicate labels are rejected before create; an ambiguous lookup fails closed.
The label file shares the session's lifetime, and an ephemeral child gets no
resumable label. This revises ADR 0087's rejection of a create id to address
observed model calls while preserving its session-id and process ownership
rules. No credential or provider configuration enters the label file.

## Evidence and rollback

The local replay script in `tests/bench/replay_tool_search.py` extracts the
recorded `grep_files` and `glob_files` argument shapes from saved sessions and
replays them against synthetic local fixture trees. Its before/after no-match
counts are reported in the worker status file; this is an offline diagnostic,
not a live inference or task-success claim. The optional experiment flag is
`none (bug fix)`. The hand-written tool-schema JSON grows from 25,309 to
25,468 bytes, a 159-byte static prefix increase, measured by decoding the C
string literals in `src/core/tools.c` from the baseline revision and this
change. There are no per-turn request-byte or reasoning-item changes.

The 197 recorded argument shapes (170 grep, 27 glob) yielded 95 and 16
no-matches with the baseline implementation on seeded fixture trees. The
changed implementation yielded 13 and 0 no-matches, respectively. The
fixtures seed a plausible match for each shape; they cannot prove the
original projects contained those matches.

Rollback is a revert of this change. Existing generated child ids remain valid
throughout. Removing the label lookup leaves stored sessions addressable by
generated id; `subagent-label` files can remain beside them harmlessly.
