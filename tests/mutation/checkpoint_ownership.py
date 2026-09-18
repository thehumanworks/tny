#!/usr/bin/env python3
"""Checkpoint behavioral mutations using the shared isolated owner runner.

The copy/serialization mutants bypass both their immediate check and the sticky
allocator safeguard. Removing just one redundant barrier is equivalent; these
mutants model actually accepting an incomplete copy/default identity. Kills
must come from checkpoint assertions, not compilation failures.
"""

from parser_ownership import main

SOURCE = "src/core/checkpoint.cpp"
MUTANTS = (
    (
        "read-only-recovery-widened",
        SOURCE,
        'check(!resolved->workspace_read_only || jget_bool(saved, "workspace_read_only", false));',
        "// intentionally ignore the inherited read-only ceiling",
    ),
    (
        "unchecked-copy",
        SOURCE,
        "check(!src || next != nullptr); // CP6 checked-copy oracle",
        "tny_alloc_scope_clear(); // intentionally accept failed copy",
    ),
    (
        "default-identity-on-oom",
        SOURCE,
        "check(value != nullptr); // CP6 serialization must not become default identity",
        'if (!value) { tny_alloc_scope_clear(); value.reset(tny_alloc_strdup("{}")); }',
    ),
    (
        "caller-mutation",
        SOURCE,
        "context recover(const tny_ctx *resolved, yyjson_val *saved) {",
        "context recover(const tny_ctx *resolved, yyjson_val *saved) {\n"
        "    if (resolved) const_cast<tny_ctx *>(resolved)->model_from_flag = !resolved->model_from_flag;",
    ),
    (
        "authority-bypass",
        SOURCE,
        'check(jget_int(saved, "perm_mode", 99) <= resolved->perm_mode &&\n'
        '          jget_int(saved, "tool_profile", -1) >= resolved->tool_profile); // CP6 authority oracle',
        "check(true); // intentionally bypass authority",
    ),
    (
        "backend-bound-bypass",
        SOURCE,
        "check(c->backend >= unresolved_backend && c->backend < TNY_BK_COUNT);",
        "check(c->backend >= unresolved_backend); // intentionally accept an invalid backend",
    ),
    (
        "stale-null-model-route",
        SOURCE,
        "    // Remove stale routing even for an explicitly absent/empty saved model.",
        "    if (!c->model || !*c->model) return; // intentionally retain stale routing\n"
        "    // Remove stale routing even for an explicitly absent/empty saved model.",
    ),
    (
        "routing-header-order",
        SOURCE,
        "if (routing_at >= 0 && routing_at < n) {",
        "if (false) { // intentionally move routing to the end",
    ),
    (
        "saved-backend-authority",
        SOURCE,
        'jget_int(saved, "backend", -1) == TNY_BK_OPENAI',
        'jget_int(saved, "backend", -1) < TNY_BK_COUNT',
    ),
    (
        "resolved-backend-authority",
        SOURCE,
        "resolved->backend == TNY_BK_OPENAI",
        "resolved->backend < TNY_BK_COUNT",
    ),
    (
        "missing-identity",
        SOURCE,
        "check(saved_identity && provider && cwd",
        "check(provider && cwd",
    ),
    (
        "fixed-field-truncation",
        SOURCE,
        "check(!text || strlen(text) < N);",
        "check(true); // intentionally truncate fixed storage",
    ),
)

if __name__ == "__main__":
    raise SystemExit(main(MUTANTS, "checkpoint assertion at line", "checkpoint"))
