# Review of #142: checkpoint C++ boundary

This review covers the supplied excerpts only. Nothing was run. The deleted `checkpoint.c`, the `tny_ctx` definition, the backend enum, `jget_*`, `path_abs`, `instructions_refresh`, `buf_t`, and `secure_free` were not shown, so they are out of view.

## Findings

### P1 — `backend` range check is either wrong or missing (`restore_number`, `restore()`)

`restore_number` applies `if constexpr (std::is_enum_v<T>) check(n >= 0 && n <= 2);` to every enum field. `perm_mode`, `tool_profile` and `image_input` also get explicit named-bound checks afterwards. `backend` gets none. Either way this is a defect in the code as written; how bad depends on a declaration I can't see:

- **If `tny_ctx::backend` is `tny_backend_id` and `TNY_BK_COUNT > 3`:** valid snapshots with backend ≥ 3 fail to restore. That breaks field/schema parity for valid input. The fixture never notices because it only uses `TNY_BK_OPENAI`.
- **If `backend` is a plain `int`:** there is no bound at all. `"backend": 99` restores and later indexes backend tables (`tny_backend_name((tny_backend_id)i)`-style lookups).

**Reproduction/test:**
- Round-trip a fixture with `backend = TNY_BK_COUNT - 1` and expect success.
- Expect `invalid(d, "backend", "-1")` and `invalid(d, "backend", "<TNY_BK_COUNT>")` to be rejected.

**Minimal fix:** remove the magic `2` from `restore_number` and add `check(c->backend >= 0 && c->backend < TNY_BK_COUNT)` next to the other three range checks.

### P2 — `finish_profile` keeps a stale Grok override header when the saved model is null or empty

The early return `!c->model || !*c->model` happens before the header strip loop. The public snapshot writes `"model": null` explicitly (`else if (public_only) add_null`), so `recover()` overwrites the resolved model with NULL. The restored context still carries `x-grok-model-override: <resolved default model>`, copied from `resolved`.

The saved identity was hashed without any override header, so `check(strcmp(saved_identity, digest) == 0)` fails and recovery returns NULL. The `models` settings key is deliberately excluded from identity so that default-model drift does not block recovery, and this path defeats that.

This is definite behaviour of the code shown. Whether the deleted C code behaved the same cannot be checked.

**Reproduction:** take `recovery_routing` and save with `model = NULL`, grok provider, and the proxy `base_url`. Resolve with `model = "new-default-model"`. Expect:
- a non-NULL result;
- `c->model == NULL`;
- no header starting with `x-grok-model-override: `.

**Minimal fix:** strip the header whenever provider is grok and the URL is the proxy. Re-add it and run the postcondition only when the model is non-empty.

### P2 (question) — `restore()` requires constructor side-products that the snapshot overwrites anyway

`check(c->settings_path && … && c->instructions_snapshot)` runs right after `allocation_ok()`, which already covers OOM. `tny_ctx_new_explicit` discards the result of `instructions_refresh`.

If `instructions_refresh` can leave `instructions_snapshot` NULL for non-OOM reasons, a valid snapshot fails to restore because of the environment rather than because of its content. Possible reasons include an unreadable instructions file or an I/O error.

**Test:** restore a valid snapshot from a cwd containing an `AGENTS.md`-style file with mode 000 and expect success. If that cannot happen, state it as an invariant of `instructions_refresh`.

### P2 (question) — authority oracle covers only `perm_mode` and `tool_profile`

`public_key` lets the saved snapshot override several fields that widen authority, and none of them are clamped against `resolved` or included in `identity()`:
- `extensions_enabled` and `tny_dir`, which together control the directory extensions are discovered and run from;
- `sandbox_mode`, `extra_dirs`, `mcp_disabled`, `no_host_registry`;
- `max_steps`. A saved `"max_steps": null` becomes 0 (unlimited) through the absent/null fallback in `restore_number`.

If the old C code did the same, parity is preserved. Even so, no test fixes the intended behaviour in either direction. Please confirm the intent and add oracles for it. For example: `reject(full, d, "extensions_enabled", "true")` when resolved has it false, or an explicit acceptance test.

### P3 — secret lifetime gaps on the new path

- **`identity()` buffer:** `secret_buffer` zeroes only the final `cap`. `jescape` growth through `realloc` leaves earlier unzeroed copies of headers, settings and secrets behind, unless `buf_t` grows securely (not shown).
- **`recover()` documents:** recovery holds all credentials in two yyjson arenas (`d` and `parsed`), and `restore_document` adds a parsed copy. `identity()` also makes mutable copies of `settings`/`repo_cfg`, on every identity computation. All of these are freed without zeroing.
- **Failure-path teardown:** `context_deleter` calls `tny_ctx_free`, which uses plain `free` for fields this file marks private: `base_url`, `auth_header_*`, `chatgpt_account_id`, `codex_base_url`, `extra_headers`. This matches normal context teardown, but it is inconsistent with the `secure_free` use in `replace_string` and `restore_array`.

### P3 — `add_entry` does not zero the new `ext_entry` slot (`extensions.c`)

`pending_failure_add` memsets its new slot; `add_entry` does not. This is safe only if `ext_entry` has exactly the two fields `name` and `path`. Otherwise, zero the slot.

### P3 (question) — a non-OOM `path_abs` failure aborts all discovery

If `path_abs` returns NULL for a dangling symlink such as `x.py`, `add_entry` returns false. `discover` then fails, and `enable_extensions` makes the whole restore or recover fail. Should non-OOM failures skip that entry instead?

### P3 — parity of absent-field defaults is unverifiable

Absent fields restore as follows:
- bools become `false`, overriding the constructor's `context_enabled`, `mcp_disabled` and `library_mode` values of `true`;
- numbers become 0, except `max_tool_result_bytes`;
- `ws_hash` becomes `""`, overriding the hash computed from `cwd`.

The tests only show that this is self-consistent. They do not show it matches the old decoder.

### Checked and found sound in these excerpts

- **Array restore:** `restore_array` keeps the successfully copied prefix owned and terminated at every failure point.
- **`finish_profile` postcondition:** it catches a silent `realloc` failure, `xstrdup` failure, and truncation inside `tny_ctx_add_extra_header`.
- **No caller mutation:** `resolved` is only ever read through a `const` pointer.
- **Exception boundaries:** each `extern "C"` function has a function-try-block. The deleters are `noexcept`, and no C callback re-enters C++.
- **Malformed input:** duplicate keys and keys with an embedded NUL are rejected.
- **Collision cleanup:** `drop_name_collisions` cannot double-free on an early return.

## Uncovered test obligations

1. **Backend bounds:** the tests listed under the P1 finding.
2. **Null-model grok recovery:** the test listed under the first P2 finding. Add two more cases: the override header not in last position in the saved context, and `resolved` carrying two override headers.
3. **Failed-scope test is vacuous:** `tny_checkpoint_context_restore(NULL)` fails on `yyjson_is_obj` regardless of scope state. To exercise `allocation_ok()`, use a valid snapshot and also call `tny_checkpoint_context` and `tny_checkpoint_public` inside a failed scope.
4. **Leak accounting is C++-only:** `tny_alloc_test_owned_live()` excludes C buffers. Leaks of `tny_ctx`, strdup'd strings, arrays, or partial `entries`/`pending_failures` on the faulted indices are invisible unless the sweep runs under LSan or `leaks`. Record which one actually ran.
5. **`reject()` compares shallowly:** it only compares the struct with `memcmp`. Add `same(c, before)` as `sweep` does.
6. **Authority-field oracles:** one test per field listed in the second P2 finding.
7. **Extension discovery edges:**
   - over `EXT_MAX_ENTRIES` entries;
   - a dangling symlink;
   - an unreadable `extensions/` directory;
   - `extensions_enabled` with a `tny_dir` that vanishes between save and restore.
8. **Identity serialisation stability:** settings containing doubles, big integers and duplicate keys must produce the same identity after the jwrite → jparse round trip. Also cover settings read with non-default flags.
9. **Number fields:** negative `max_steps`, negative `extension_timeout_ms`, and `mcp_import_order` values outside `{1,2,4,8}` or duplicated. Decide whether these fail closed.

## Verdict

Not clean. The backend range check is a P1 defect. The null-model Grok recovery is a P2 defect, and two P2 questions about parity and authority remain open.

The all-or-none copy and cleanup logic, the exception boundary, array termination, deep copy, and caller immutability look correct in the code shown.

**What cannot be proved from these excerpts:**
- parity with the deleted `checkpoint.c` for defaults, authority handling and null handling;
- that yyjson's `jallocator` and the C calls `realloc`, `xstrdup`, `path_abs` and `buf_*` are all routed through the injected allocator, which determines whether the sweep's fault count is complete;
- absence of leaks on C allocations;
- behaviour if allocating the exception itself fails under real OOM, since `__cxa_allocate_exception` bypasses the allocator wrappers and terminates once its emergency pool is exhausted;
- whether `buf_t` and `secure_free` zero memory when a buffer grows;
- the `tny_ctx` field types that `restore_number` is instantiated with.
