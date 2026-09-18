#!/usr/bin/env python3
"""Launch snapshot behavioral mutations in isolated source/object copies."""

from parser_ownership import main

SOURCE = "src/core/subagent_plan.cpp"
MUTANTS = (
    (
        "borrowed-selector",
        SOURCE,
        "argv[i] = copy(args[i]);",
        "argv[i] = const_cast<char *>(args[i]);",
    ),
    (
        "borrowed-environment",
        SOURCE,
        "envp[used++] = copy(*e);",
        "envp[used++] = *e;",
    ),
    (
        "missing-wipe",
        SOURCE,
        "if (bytes) secure_zero(bytes.get(), size);",
        "(void)size;",
    ),
    (
        "first-string-only-wipe",
        SOURCE,
        "secure_zero(bytes.get(), size);",
        "secure_zero(bytes.get(), std::strlen(bytes.get()));",
    ),
    (
        "destroy-before-proof",
        SOURCE,
        "auto next = tny::make_owned<tny_subagent_plan_owner>",
        "tny_subagent_plan_free(plan);\n"
        "        auto next = tny::make_owned<tny_subagent_plan_owner>",
    ),
    (
        "leak-replaced-owner",
        SOURCE,
        "        tny_subagent_plan_free(plan);",
        "        // intentionally abandon the previous owner",
    ),
    (
        "permission-escalation",
        SOURCE,
        '{"TNY_NESTED_MODE", tny_perm_mode_name(ctx.perm_mode)}',
        '{"TNY_NESTED_MODE", "yolo"}',
    ),
    (
        "wrong-provider",
        SOURCE,
        "arg(tny_provider_name(&ctx));",
        'arg("openai");',
    ),
    (
        "missing-key",
        SOURCE,
        "const bool key = ctx.api_key && *ctx.api_key;",
        "const bool key = false;",
    ),
    (
        "ambient-account-paired-with-selected-token",
        SOURCE,
        '((token || account) && env_named(entry, "CHATGPT_ACCOUNT_ID"))',
        '(account && env_named(entry, "CHATGPT_ACCOUNT_ID"))',
    ),
    (
        "oom-reported-as-success",
        SOURCE,
        "errno = ENOMEM;\n        return -1;",
        "errno = ENOMEM;\n        return 0;",
    ),
)

if __name__ == "__main__":
    raise SystemExit(main(MUTANTS, "subagent assertion at line", "subagent"))
