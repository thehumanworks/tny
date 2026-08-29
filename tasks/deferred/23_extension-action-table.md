# 23 — Data-driven extension action kinds

Medium. From the complexity review. Independent of 20–22.

`src/core/extensions.c` `append_action` maps string kinds with a long
`if/else if` plus per-kind validation (capability, content, reason,
arguments). `fold_extension_result` in `src/core/runtime.c` then walks
actions multiple times with the same “wrong phase → status” pattern.

## Work

- Replace the kind `strcmp` ladder with a static table, e.g.
  `{ "tool_rewrite", TOOL_REWRITE, CAP_TOOL_PRE_REWRITE, NEED_ARGS, 0 }`,
  plus one special case for `permission_decision` (allow_once / deny /
  abstain).
- Fold `fold_extension_result` to a single pass over actions dispatching
  on `kind`, preserving phase checks and status codes.
- New action kinds become a table row, not another nested branch.
- Keep existing runtime/extension unit tests; add one case that an
  unknown kind still yields `invalid_action` and that a capability-
  gated kind still fails closed when unsupported.

## Acceptance

- Extension fold/stop/continue/prompt-transform tests in
  `tests/test_runtime.c` (and integration extension hosts) stay green.
- Unsupported / unknown action kinds still fail closed with the same
  error codes.
- Adding a kind does not require editing four sequential scans in
  `fold_extension_result`.
