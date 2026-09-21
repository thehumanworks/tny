# 21 — Ephemeral libtny runtimes write the undo journal into the workspace

Medium (contract). Found while writing `examples/sdk/`. Independent.

`docs/libtny.md` says that with `persistence == 0` and an empty `state_dir`
"libtny creates no settings, sessions, history, or other state path". A
file-writing tool breaks that: `tools_undo_record` (`src/core/tools_fs.c`)
unconditionally `mkdir_p`s `env->session->dir` and writes `undo.json` /
`undo.blob` there. With no state directory that path is relative, and the
journal appears as `<workspace>/sessions/<hash>/<session>/undo.json` — inside
the tree the agent is editing, where it is picked up by later `list_files`,
`git status`, and code generation output.

Root cause: `src/lib/tny.c` creates the context with
`tny_ctx_new_explicit(workspace, state_dir ? state_dir : workspace)`, so an
omitted state directory silently becomes the workspace.

Reproduce: Python or Node SDK, `RuntimeConfig(workspace=W)` with no
`state_dir`, any turn that calls `write_file`; then `ls W/sessions`.
Passing an explicit `state_dir` sends the journal there instead, which is the
workaround `examples/sdk` uses (`_state_dir` / `scratchStateDir`).

## Work

- Decide the ephemeral contract: either skip the on-disk undo journal when the
  session is not persistent (undo then reports nothing to undo), or keep it in
  memory. Do not materialize a state path the embedder did not provide.
- Add an ABI/SDK test: ephemeral runtime, one `write_file` turn, assert the
  workspace contains only the written file and that no `sessions/` directory
  exists under the workspace or the process cwd.
- Once fixed, drop the scratch state directory from `examples/sdk` helpers and
  the matching README bullet.

## Acceptance

- The new test fails before the change and passes after.
- Persistent sessions keep working `/undo` behaviour unchanged.
- `docs/libtny.md` matches the implemented behaviour.
