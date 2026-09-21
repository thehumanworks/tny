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
bool inherited(const char *entry, bool token, bool account, bool acp, const tny_ctx &ctx) noexcept {
    if (acp) {
        if (env_named(entry, "OPENAI_API_KEY") || env_named(entry, "OPENAI_BASE_URL") ||
            env_named(entry, "CHATGPT_ACCESS_TOKEN") || env_named(entry, "CHATGPT_ACCOUNT_ID") ||
            env_named(entry, "TNY_CODEX_BASE_URL"))
            return false;
        /* Do not pass a parent's resolved native credentials through an
         * arbitrary custom environment carrier. Unrelated tool credentials
         * and the adapter's own account configuration remain available. */
        const char *value = std::strchr(entry, '=');
        const char *native_values[] = {ctx.api_key, ctx.chatgpt_token, ctx.chatgpt_account_id,
                                       ctx.base_url, ctx.codex_base_url};
        if (value && value[1])
            for (const char *secret : native_values)
                if (secret && *secret && std::strcmp(value + 1, secret) == 0) return false;
    }
    return !(env_named(entry, TNY_SUBAGENT_KEY_ENV) || env_named(entry, TNY_SUBAGENT_URL_ENV) ||
             env_named(entry, "TNY_NESTED") || env_named(entry, "TNY_NESTED_MODE") ||
             env_named(entry, "TNY_TEAM_RUN") || env_named(entry, "TNY_TEAM_TASK") ||
             env_named(entry, "TNY_TEAM_ATTEMPT") || env_named(entry, "TNY_TEAM_CAPABILITY") ||
             env_named(entry, "TNY_TEAM_READ_ONLY") || env_named(entry, "TNY_TOOLS") ||
             env_named(entry, "TNY_ACP_REQUIRE_TOOLS_AUTHORITY") ||
             env_named(entry, "TNY_ACP_FROZEN_COMMAND") ||
             env_named(entry, "TNY_ACP_CLEANUP_FILE") || env_named(entry, "TNY_PERMISSION_MODE") ||
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
    std::array<char *, 164> argv{};
    tny::vector<char *> envp;
    secret_block storage;

    tny_subagent_plan_owner(const tny_ctx &ctx, const char *resume_id, yyjson_val *selection,
                            const char *exe) {
        const char *provider = jget_str(selection, "provider");
        const bool parent = tny_subagent_provider_is_parent(&ctx, provider);
        const char *model = jget_str(selection, "model");
        const char *effort = jget_str(selection, "effort");
        if (parent) provider = tny_provider_name(&ctx);
        const bool acp = std::strcmp(provider, "acp") == 0 ||
                         std::strncmp(provider, "acp@", 4) == 0 ||
                         std::strncmp(provider, "acp:", 4) == 0;
        if (!model && parent) model = ctx.model;
        if (!model && parent && ctx.backend == TNY_BK_ACP) model = "";
        if (!effort && parent)
            effort =
                ctx.reasoning_effort && *ctx.reasoning_effort ? ctx.reasoning_effort : "default";
        // A different selector is resolved by the child CLI, including named
        // profiles and hosts. Never apply the parent's resolved connection or
        // subscription credentials to it. Ambient user auth remains available.
        const bool key = parent && ctx.backend == TNY_BK_OPENAI && ctx.api_key && *ctx.api_key;
        const bool url = parent && ctx.backend == TNY_BK_OPENAI && ctx.base_url && *ctx.base_url;
        const bool token =
            parent && ctx.backend == TNY_BK_OPENAI && ctx.chatgpt_token && *ctx.chatgpt_token;
        const bool account = parent && ctx.backend == TNY_BK_OPENAI && ctx.chatgpt_account_id &&
                             *ctx.chatgpt_account_id;
        std::array<const char *, 164> args{};
        size_t argc = 0;
        const auto arg = [&](const char *text) {
            if (argc >= args.size() - 1 || !text) throw std::bad_alloc();
            args[argc++] = text;
        };
        arg(exe);
        arg("--cwd");
        arg(ctx.cwd);
        arg("--provider");
        arg(provider);
        char agent_count[16];
        if (parent && ctx.backend == TNY_BK_ACP && ctx.agent_argv) {
            size_t count = 0;
            while (count <= 128 && ctx.agent_argv[count]) ++count;
            if (!count || count > 128) throw std::bad_alloc();
            std::snprintf(agent_count, sizeof agent_count, "%zu", count);
            arg("--acp-agent-argv");
            arg(agent_count);
            for (size_t i = 0; i < count; ++i) arg(ctx.agent_argv[i]);
        }
        if (key) {
            arg("--api-key-env");
            arg(TNY_SUBAGENT_KEY_ENV);
        }
        if (url) {
            arg("--base-url-env");
            arg(TNY_SUBAGENT_URL_ENV);
        }
        if (parent && ctx.backend == TNY_BK_OPENAI && ctx.wire_api) {
            arg("--wire-api");
            arg(tny_wire_is_chat(ctx.wire_api) ? "chat" : "responses");
        }
        if (model) {
            arg("--model");
            arg(model);
        }
        if (effort && *effort) {
            arg("--effort");
            arg(effort);
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
        if (ctx.no_self_improve) arg("--no-self-improve");
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
            {"TNY_ACP_REQUIRE_TOOLS_AUTHORITY", ctx.acp_require_tools_authority ? "1" : nullptr},
            {"TNY_ACP_FROZEN_COMMAND",
             parent && ctx.backend == TNY_BK_ACP && ctx.agent_argv ? "1" : nullptr},
        };
        size_t total = 0, envc = 0;
        for (size_t i = 0; i < argc; ++i) {
            add_size(total, std::strlen(args[i]));
            add_size(total, 1);
        }
        for (char **e = environ; e && *e; ++e) {
            if (!inherited(*e, token, account, acp, ctx)) continue;
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
            if (inherited(*e, token, account, acp, ctx)) envp[used++] = copy(*e);
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
    return tny_subagent_plan_build_selected(env, resume_id, nullptr, plan);
}
extern "C" int tny_subagent_plan_build_selected(const tools_env *env, const char *resume_id,
                                                yyjson_val *args, tny_subagent_plan *plan) {
    try {
        tny::c_string exe(tny_process_self_path());
        if (!exe) return -1; // preserve ENOTSUP on wasm
        auto next = tny::make_owned<tny_subagent_plan_owner>(*env->ctx, resume_id, args, exe.get());
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
