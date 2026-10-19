# 0173 — Local interactive shell mode

Date: 2026-09-25
Status: accepted

## Decision

An empty TUI composer accepts `!` as a mode switch. Subsequent Enter keys
execute commands in the host's `$SHELL` (absolute path, otherwise `/bin/sh`),
inheriting tny's current working directory. The command runs in a child with
stdin redirected from `/dev/null`. The TUI polls its merged stdout/stderr and
renders it as it arrives; no provider or permission-tool request is involved.
The shell has the user's own host permissions, so this mode is intentional
local execution, not a sandbox. Esc, Ctrl-C or an empty Enter returns to the
agent composer. Exiting the TUI stops and reaps an active shell child.

Command text, output and exit status are accumulated for the next agent user
message, including queued and steered messages. Builtin slash commands do not
consume the disclosure. The record is marked as untrusted data. Its output is
capped at 64 KiB for context (full output still streams on screen); additional
commands are refused if their command text and status would not fit. A running
command blocks prompt submission so incomplete output cannot be sent. On
browser/wasm, execution fails cleanly with an unsupported message; no browser
JavaScript command runner or remote execution is added.

## Verification

The detached SMT model was retired on 2026-10-19: its equations had no
mechanical link to production and could not detect an implementation regression.
`tests/test_tui.c` exercises the real child,
merged streams, cwd, exit status, nonblocking EOF, disclosure formatting and
space guard. `tests/integration/test_tui.py` checks the actual next provider
request and the local command path without credentials.
`tests/integration/test_wasm_doctor.py` host-links the wasm seam to check the
clean refusal; actual wasm link/runtime behavior remains a CI check.
These checks do not prove the C implementation or the host shell itself.
