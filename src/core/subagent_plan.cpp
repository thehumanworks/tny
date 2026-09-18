/* Private launch snapshot. Process control stays in C (ADR 0133). */
extern "C" {
#include "core/subagent.h"
#include "util/process.h"
}
#include "util/ownership.hpp"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

extern char **environ;

namespace {
bool env_named(const char *entry, const char *name) noexcept {
    size_t n = std::strlen(name);
    return std::strncmp(entry, name, n) == 0 && entry[n] == '=';
}
bool inherited(const char *entry, bool token, bool account) noexcept {
    return !(env_named(entry, TNY_SUBAGENT_KEY_ENV) || env_named(entry, TNY_SUBAGENT_URL_ENV) ||
             env_named(entry, "TNY_NESTED") || env_named(entry, "TNY_NESTED_MODE") ||
             env_named(entry, "TNY_TEAM_RUN") || env_named(entry, "TNY_TEAM_TASK") ||
             env_named(entry, "TNY_TEAM_ATTEMPT") || env_named(entry, "TNY_TEAM_CAPABILITY") ||
             env_named(entry, "TNY_TEAM_READ_ONLY") || env_named(entry, "TNY_TOOLS") ||
             env_named(entry, "TNY_PERMISSION_MODE") ||
             (token && env_named(entry, "CHATGPT_ACCESS_TOKEN")) ||
             ((token || account) && env_named(entry, "CHATGPT_ACCOUNT_ID")));
}
void add_size(size_t &total, size_t n) {
    if (n > std::numeric_limits<size_t>::max() - total) throw std::bad_alloc();
    total += n;
}
/* One exact allocation before any secret is copied. Never grow or move the
 * bytes. Wipe the entire block, not strlen(first entry), on every release. */
struct secret_block {
    tny::c_string bytes;
    size_t size = 0;
    secret_block() = default;
    secret_block(const secret_block &) = delete;
    secret_block &operator=(const secret_block &) = delete;
    ~secret_block() noexcept {
        if (bytes) secure_zero(bytes.get(), size);
    }
    void allocate(size_t n) {
        bytes.reset(static_cast<char *>(tny_alloc_malloc(n)));
        if (!bytes) throw std::bad_alloc();
        size = n;
    }
};
struct assignment {
    const char *name;
    const char *value;
};
} // namespace

struct tny_subagent_plan_owner {
    std::array<char *, 32> argv{};
    tny::vector<char *> envp;
    secret_block storage;

    tny_subagent_plan_owner(const tny_ctx &ctx, const char *resume_id, const char *exe) {
        const bool key = ctx.api_key && *ctx.api_key;
        const bool url = ctx.base_url && *ctx.base_url;
        const bool token = ctx.chatgpt_token && *ctx.chatgpt_token;
        const bool account = ctx.chatgpt_account_id && *ctx.chatgpt_account_id;
        std::array<const char *, 32> args{};
        size_t argc = 0;
        const auto arg = [&](const char *text) {
            if (argc >= args.size() - 1 || !text) throw std::bad_alloc();
            args[argc++] = text;
        };
        arg(exe);
        arg("--cwd");
        arg(ctx.cwd);
        arg("--provider");
        arg(tny_provider_name(&ctx));
        if (key) {
            arg("--api-key-env");
            arg(TNY_SUBAGENT_KEY_ENV);
        }
        if (url) {
            arg("--base-url-env");
            arg(TNY_SUBAGENT_URL_ENV);
        }
        if (ctx.wire_api) {
            arg("--wire-api");
            arg(tny_wire_is_chat(ctx.wire_api) ? "chat" : "responses");
        }
        if (ctx.model) {
            arg("--model");
            arg(ctx.model);
        }
        if (ctx.reasoning_effort && *ctx.reasoning_effort) {
            arg("--effort");
            arg(ctx.reasoning_effort);
        }
        char steps[16];
        if (ctx.max_steps > 0) {
            std::snprintf(steps, sizeof steps, "%d", ctx.max_steps);
            arg("--max-steps");
            arg(steps);
        }
        arg("--permission-mode");
        arg(tny_perm_mode_name(ctx.perm_mode));
        if (ctx.no_save) arg("--ephemeral");
        arg("ask");
        arg("--json");
        arg("--stdin");
        if (resume_id) {
            arg("--resume-id");
            arg(resume_id);
        }
        // Fixed upper bound: four optional carriers and three ceilings.
        const assignment overrides[] = {
            {TNY_SUBAGENT_KEY_ENV, key ? ctx.api_key : nullptr},
            {TNY_SUBAGENT_URL_ENV, url ? ctx.base_url : nullptr},
            {"CHATGPT_ACCESS_TOKEN", token ? ctx.chatgpt_token : nullptr},
            {"CHATGPT_ACCOUNT_ID", account ? ctx.chatgpt_account_id : nullptr},
            {"TNY_NESTED", "1"},
            {"TNY_NESTED_MODE", tny_perm_mode_name(ctx.perm_mode)},
            {"TNY_TOOLS", tny_tool_profile_name(ctx.tool_profile)},
            {"TNY_TEAM_READ_ONLY", ctx.workspace_read_only ? "1" : nullptr},
        };
        size_t total = 0, envc = 0;
        for (size_t i = 0; i < argc; ++i) {
            add_size(total, std::strlen(args[i]));
            add_size(total, 1);
        }
        for (char **e = environ; e && *e; ++e) {
            if (!inherited(*e, token, account)) continue;
            add_size(total, std::strlen(*e));
            add_size(total, 1);
            add_size(envc, 1);
        }
        for (const auto &entry : overrides) {
            if (!entry.value) continue;
            add_size(total, std::strlen(entry.name));
            add_size(total, std::strlen(entry.value));
            add_size(total, 2); // '=' and NUL
            add_size(envc, 1);
        }
        add_size(envc, 1); // terminating null pointer
        envp.resize(envc);
        storage.allocate(total);
        // No allocation or throwing operation after secret bytes enter storage.
        char *out = storage.bytes.get();
        const auto copy = [&](const char *text) noexcept {
            char *start = out;
            size_t n = std::strlen(text) + 1;
            std::memcpy(out, text, n);
            out += n;
            return start;
        };
        for (size_t i = 0; i < argc; ++i) argv[i] = copy(args[i]);
        size_t used = 0;
        for (char **e = environ; e && *e; ++e)
            if (inherited(*e, token, account)) envp[used++] = copy(*e);
        for (const auto &entry : overrides) {
            if (!entry.value) continue;
            envp[used++] = out;
            size_t n = std::strlen(entry.name);
            std::memcpy(out, entry.name, n);
            out += n;
            *out++ = '=';
            copy(entry.value);
        }
    }
};

static_assert(!std::is_copy_constructible_v<tny_subagent_plan_owner>);
static_assert(!std::is_move_constructible_v<tny_subagent_plan_owner>);
static_assert(std::is_nothrow_destructible_v<tny_subagent_plan_owner>);

extern "C" int tny_subagent_plan_build(const tools_env *env, const char *resume_id,
                                       tny_subagent_plan *plan) {
    try {
        tny::c_string exe(tny_process_self_path());
        if (!exe) return -1; // preserve ENOTSUP on wasm
        auto next = tny::make_owned<tny_subagent_plan_owner>(*env->ctx, resume_id, exe.get());
        // Publish only a complete snapshot; a failed rebuild keeps the old one.
        tny_subagent_plan_free(plan);
        plan->argv = next->argv.data();
        plan->envp = next->envp.data();
        plan->owner = next.release();
        return 0;
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }
}
extern "C" void tny_subagent_plan_free(tny_subagent_plan *plan) {
    if (!plan) return;
    tny::owned<tny_subagent_plan_owner> owner(plan->owner);
    *plan = {};
}
