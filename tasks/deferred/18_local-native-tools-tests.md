# 18 — Test local FS/shell tools and result handles

Medium. From the test-depth review. Independent of 15–17; do not route
these cases through the fake SSH path in `tests/test_ssh.c`.

`file_tools_round_trip` is strong for the **remote** script path.
Local execution is almost only `local_when_not_attached` (one
`write_file`). `read_tool_result` and `ask_user_question` have no
tests. `memory` is tested only for ephemeral reject.

## Work

- Add a `tools_execute` (or equivalent) unit suite with `ssh_host`
  unset: `list_files` / `glob_files` / `grep_files` / `read_file` /
  `write_file` / `edit_file` / `copy_file` / `rename_file` /
  `delete_file` / `create_folder` / `file_info` / `semantic_search` /
  `open_file` against a temp workspace (quoting, cwd, tilde, sandbox
  mode if it changes local behavior).
- Cover local `terminal`: timeout, background job fields (pid, cwd, log
  path), and that the process is reaped. Keep remote coverage in
  `test_ssh.c`; do not duplicate the SSH script compiler here.
- Test `read_tool_result` on a bounded preview handle (byte range and
  literal search) and `ask_user_question` at least for the
  noninteractive `ask` path (must not hang; documented error/exit).
- One non-ephemeral `memory set`/`get`/`list` round-trip against a fake
  `TNY_HOME` in addition to the existing ephemeral reject.

## Acceptance

- Breaking local `edit_file` or `read_tool_result` range reads fails
  `make test` without involving SSH.
- `ask_user_question` under `tny ask` does not block the unit/integration
  suite.
- `test_ssh.c` remote round-trip remains the SSH contract; local cases
  live in a distinct test file.
