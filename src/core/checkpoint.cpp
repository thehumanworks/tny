/* Private checkpoint ownership; schema and C facade remain unchanged (ADR0126). */
extern "C" {
#include "core/checkpoint.h"
#include "core/extensions.h"
#include "util/util.h"
}
#include "json/ownership.hpp"
#include <climits>
#include <cstring>
#include <limits>
#include <type_traits>

namespace {
struct invalid_snapshot {};
void check(bool ok) {
    if (!ok) throw invalid_snapshot{};
}
void allocation_ok() { check(!tny_alloc_scope_failed()); }
struct context_deleter {
    void operator()(tny_ctx *ctx) const noexcept { tny_ctx_free(ctx); }
};
using context = std::unique_ptr<tny_ctx, context_deleter>;
struct secret_deleter {
    void operator()(char *p) const noexcept { secure_free(p); }
};
using secret_string = std::unique_ptr<char, secret_deleter>;
struct secret_buffer {
    buf_t value{};
    secret_buffer() { buf_init(&value); }
    ~secret_buffer() {
        if (value.data) secure_zero(value.data, value.cap);
        buf_free(&value);
    }
    secret_buffer(const secret_buffer &) = delete;
    secret_buffer &operator=(const secret_buffer &) = delete;
};
struct string_field {
    const char *name;
    char *tny_ctx::*member;
    bool private_value;
};
constexpr string_field strings[] = {
    {"cwd", &tny_ctx::cwd, false},
    {"provider_name", &tny_ctx::provider_name, false},
    {"model", &tny_ctx::model, false},
    {"base_url", &tny_ctx::base_url, true},
    {"api_key", &tny_ctx::api_key, true},
    {"auth_header_name", &tny_ctx::auth_header_name, true},
    {"auth_header_prefix", &tny_ctx::auth_header_prefix, true},
    {"max_tokens_field", &tny_ctx::max_tokens_field, false},
    {"wire_api", &tny_ctx::wire_api, false},
    {"output_schema", &tny_ctx::output_schema, false},
    {"bridge_bin", &tny_ctx::bridge_bin, false},
    {"xai_api_key", &tny_ctx::xai_api_key, true},
    {"chatgpt_token", &tny_ctx::chatgpt_token, true},
    {"chatgpt_account_id", &tny_ctx::chatgpt_account_id, true},
    {"codex_base_url", &tny_ctx::codex_base_url, true},
    {"ssh_host", &tny_ctx::ssh_host, false},
    {"ssh_cwd", &tny_ctx::ssh_cwd, false},
    {"ssh_control", &tny_ctx::ssh_control, false},
    {"service_tier", &tny_ctx::service_tier, false},
    {"system_prompt", &tny_ctx::system_prompt, false},
    {"task_name", &tny_ctx::task_name, false},
    {"task_source", &tny_ctx::task_source, false},
    {"task_instructions", &tny_ctx::task_instructions, false},
    {"reasoning_effort", &tny_ctx::reasoning_effort, false},
    {"instructions_snapshot", &tny_ctx::instructions_snapshot, false},
    {"sandbox_mode", &tny_ctx::sandbox_mode, false},
    {"tny_dir", &tny_ctx::tny_dir, false},
    {"settings_path", &tny_ctx::settings_path, false},
};
struct bool_field {
    const char *name;
    bool tny_ctx::*member;
};
constexpr bool_field bools[] = {
    {"model_from_flag", &tny_ctx::model_from_flag},
    {"json_out", &tny_ctx::json_out},
    {"no_save", &tny_ctx::no_save},
    {"no_color", &tny_ctx::no_color},
    {"force_color", &tny_ctx::force_color},
    {"library_mode", &tny_ctx::library_mode},
    {"prompt_optimisation", &tny_ctx::prompt_optimisation},
    {"extensions_enabled", &tny_ctx::extensions_enabled},
    {"no_host_registry", &tny_ctx::no_host_registry},
    {"service_tier_explicit", &tny_ctx::service_tier_explicit},
    {"service_tier_from_settings", &tny_ctx::service_tier_from_settings},
    {"task_explicit", &tny_ctx::task_explicit},
    {"effort_explicit", &tny_ctx::effort_explicit},
    {"effort_from_settings", &tny_ctx::effort_from_settings},
    {"context_enabled", &tny_ctx::context_enabled},
    {"instructions_snapshot_ready", &tny_ctx::instructions_snapshot_ready},
    {"mcp_disabled", &tny_ctx::mcp_disabled},
    {"mcp_import_warned", &tny_ctx::mcp_import_warned},
};

bool absent(yyjson_val *v) { return !v || yyjson_is_null(v); }
const char *optional_string(yyjson_val *v) {
    if (absent(v)) return nullptr;
    check(yyjson_is_str(v));
    const char *text = yyjson_get_str(v);
    check(strlen(text) == yyjson_get_len(v));
    return text;
}
void replace_string(char *&dest, const char *src) {
    secret_string next(src ? tny_alloc_strdup(src) : nullptr);
    check(!src || next != nullptr); // CP6 checked-copy oracle
    secure_free(dest);
    dest = next.release();
}
template <size_t N> void restore_fixed(char (&dest)[N], yyjson_val *v) {
    const char *text = optional_string(v);
    check(!text || strlen(text) < N);
    snprintf(dest, N, "%s", text ? text : "");
}
template <class T> void restore_number(T &dest, yyjson_val *v, T fallback = {}) {
    static_assert(!std::is_enum_v<T>);
    if (absent(v)) {
        dest = fallback;
        return;
    }
    check(yyjson_is_int(v));
    if constexpr (std::is_unsigned_v<T>) {
        check(!yyjson_is_sint(v) || yyjson_get_sint(v) >= 0);
        check(yyjson_get_uint(v) <= std::numeric_limits<T>::max());
        dest = static_cast<T>(yyjson_get_uint(v));
    } else {
        static_assert(std::is_same_v<T, int>);
        check(!yyjson_is_uint(v) || yyjson_get_uint(v) <= INT_MAX);
        const int64_t n = yyjson_get_sint(v);
        check(n >= INT_MIN && n <= INT_MAX);
        dest = static_cast<T>(n);
    }
}
template <class T> void restore_enum(T &dest, yyjson_val *v, T lower, T upper) {
    int number = 0;
    restore_number(number, v);
    check(number >= static_cast<int>(lower) && number <= static_cast<int>(upper));
    dest = static_cast<T>(number);
}
size_t array_size(yyjson_val *v) {
    check(absent(v) || yyjson_is_arr(v));
    size_t n = yyjson_arr_size(v);
    check(n <= INT_MAX && n < SIZE_MAX / sizeof(char *));
    return n;
}
void restore_array(char **&dest, int &count, yyjson_val *v) {
    const size_t n = array_size(v);
    for (int i = 0; i < count; ++i) secure_free(dest[i]);
    free(dest);
    dest = nullptr;
    count = 0;
    dest = static_cast<char **>(tny_alloc_calloc(n + 1, sizeof(char *)));
    check(dest != nullptr);
    for (size_t i = 0; i < n; ++i) {
        const char *text = optional_string(yyjson_arr_get(v, i));
        check(text != nullptr);
        dest[i] = tny_alloc_strdup(text);
        check(dest[i] != nullptr);
        ++count; // context owns exactly the successfully copied prefix
    }
}
void encode_array(yyjson_mut_doc *d, yyjson_mut_val *r, const char *key, char *const *values,
                  int n) {
    check(n >= 0 && (n == 0 || values));
    auto *arr = tny::required(yyjson_mut_arr(d));
    for (int i = 0; i < n; ++i) {
        check(values[i] != nullptr);
        check(yyjson_mut_arr_add_strcpy(d, arr, values[i]));
    }
    check(yyjson_mut_obj_add_val(d, r, key, arr));
}
int header_count(const tny_ctx *c) {
    size_t n = 0;
    while (c->extra_headers && c->extra_headers[n]) {
        check(n < INT_MAX);
        ++n;
    }
    return static_cast<int>(n);
}
yyjson_mut_val *encode(yyjson_mut_doc *d, const tny_ctx *c, bool public_only) {
    check(d && c);
    allocation_ok();
    auto *r = tny::required(yyjson_mut_obj(d));
    for (const auto &f : strings) {
        if (public_only && f.private_value) continue;
        const char *text = c->*(f.member);
        if (public_only && f.member == &tny_ctx::provider_name) text = tny_provider_name(c);
        if (text) check(yyjson_mut_obj_add_strcpy(d, r, f.name, text));
        else if (public_only) check(yyjson_mut_obj_add_null(d, r, f.name));
    }
    check(memchr(c->ws_hash, 0, sizeof c->ws_hash) != nullptr);
    check(yyjson_mut_obj_add_strcpy(d, r, "ws_hash", c->ws_hash));
    check(memchr(c->ssh_port, 0, sizeof c->ssh_port) != nullptr);
    check(yyjson_mut_obj_add_strcpy(d, r, "ssh_port", c->ssh_port));
    check(memchr(c->task_digest, 0, sizeof c->task_digest) != nullptr);
    check(yyjson_mut_obj_add_strcpy(d, r, "task_digest", c->task_digest));
    check(memchr(c->instructions_digest, 0, sizeof c->instructions_digest) != nullptr);
    check(yyjson_mut_obj_add_strcpy(d, r, "instructions_digest", c->instructions_digest));
    for (const auto &f : bools) check(yyjson_mut_obj_add_bool(d, r, f.name, c->*(f.member)));
    check(yyjson_mut_obj_add_int(d, r, "backend", c->backend));
    check(yyjson_mut_obj_add_int(d, r, "max_extension_iterations", c->max_extension_iterations));
    check(yyjson_mut_obj_add_int(d, r, "extension_timeout_ms", c->extension_timeout_ms));
    check(yyjson_mut_obj_add_int(d, r, "max_steps", c->max_steps));
    check(yyjson_mut_obj_add_int(d, r, "perm_mode", c->perm_mode));
    check(yyjson_mut_obj_add_int(d, r, "tool_profile", c->tool_profile));
    check(yyjson_mut_obj_add_int(d, r, "image_input", c->image_input));
    check(yyjson_mut_obj_add_uint(d, r, "max_tool_result_bytes", c->max_tool_result_bytes));
    check(yyjson_mut_obj_add_uint(d, r, "mcp_import_mask", c->mcp_import_mask));
    encode_array(d, r, "extra_dirs", c->extra_dirs, c->n_extra_dirs);
    encode_array(d, r, "instruction_paths", c->instruction_paths, c->n_instruction_paths);
    if (!public_only) encode_array(d, r, "extra_headers", c->extra_headers, header_count(c));
    check(c->n_mcp_import_sources >= 0 && c->n_mcp_import_sources <= 4);
    auto *order = tny::required(yyjson_mut_arr(d));
    for (int i = 0; i < c->n_mcp_import_sources; ++i)
        check(yyjson_mut_arr_add_uint(d, order, c->mcp_import_order[i]));
    check(yyjson_mut_obj_add_val(d, r, "mcp_import_order", order));
    if (!public_only) {
        if (c->settings)
            check(yyjson_mut_obj_add_val(
                d, r, "settings",
                tny::required(yyjson_val_mut_copy(d, yyjson_doc_get_root(c->settings)))));
        if (c->repo_cfg)
            check(yyjson_mut_obj_add_val(
                d, r, "repo_cfg",
                tny::required(yyjson_val_mut_copy(d, yyjson_doc_get_root(c->repo_cfg)))));
    }
    allocation_ok();
    return r;
}
void restore_document(yyjson_doc *&dest, yyjson_val *v) {
    if (!v) return;
    secret_string json(tny::required(jwrite_val(v)));
    tny::document next(tny::required(jparse(json.get(), strlen(json.get()))));
    yyjson_doc_free(dest);
    dest = next.release();
}
void enable_extensions(tny_ctx *c) {
    if (c->extensions_enabled) {
        c->extensions = tny_extensions_new(c->tny_dir, c->cwd, c->extension_timeout_ms);
        check(c->extensions != nullptr);
        allocation_ok();
    }
}
context restore(yyjson_val *r) {
    allocation_ok();
    check(yyjson_is_obj(r));
    const char *cwd = optional_string(jget(r, "cwd"));
    const char *dir = optional_string(jget(r, "tny_dir"));
    check(cwd && dir);
    context c(tny::required(tny_ctx_new_explicit(cwd, dir)));
    // The C constructor permits optional defaults to fail. None may mask OOM here.
    allocation_ok();
    check(c->settings_path && c->provider_name && c->sandbox_mode && c->base_url &&
          c->auth_header_name && c->auth_header_prefix && c->bridge_bin && c->cursor_config &&
          c->instructions_snapshot);
    for (const auto &f : strings)
        replace_string(c.get()->*(f.member), optional_string(jget(r, f.name)));
    restore_fixed(c->ws_hash, jget(r, "ws_hash"));
    restore_fixed(c->ssh_port, jget(r, "ssh_port"));
    restore_fixed(c->task_digest, jget(r, "task_digest"));
    restore_fixed(c->instructions_digest, jget(r, "instructions_digest"));
    for (const auto &f : bools) {
        auto *v = jget(r, f.name);
        check(absent(v) || yyjson_is_bool(v));
        c.get()->*(f.member) = yyjson_get_bool(v);
    }
    restore_number(c->backend, jget(r, "backend"));
    // tny_ctx_load uses -1 until provider resolution. Private snapshots have
    // always round-tripped that sentinel; public recovery still requires OpenAI.
    constexpr int unresolved_backend = -1;
    check(c->backend >= unresolved_backend && c->backend < TNY_BK_COUNT);
    restore_number(c->max_extension_iterations, jget(r, "max_extension_iterations"));
    restore_number(c->extension_timeout_ms, jget(r, "extension_timeout_ms"));
    restore_number(c->max_steps, jget(r, "max_steps"));
    restore_enum(c->perm_mode, jget(r, "perm_mode"), TNY_MODE_ASK, TNY_MODE_YOLO);
    restore_enum(c->tool_profile, jget(r, "tool_profile"), TNY_TOOLS_ALL, TNY_TOOLS_TERMINAL);
    restore_enum(c->image_input, jget(r, "image_input"), TNY_IMAGE_INPUT_UNKNOWN,
                 TNY_IMAGE_INPUT_CONFIGURED_UNSUPPORTED);
    restore_number(c->max_tool_result_bytes, jget(r, "max_tool_result_bytes"), size_t{32768});
    restore_number(c->mcp_import_mask, jget(r, "mcp_import_mask"));
    restore_array(c->extra_dirs, c->n_extra_dirs, jget(r, "extra_dirs"));
    restore_array(c->instruction_paths, c->n_instruction_paths, jget(r, "instruction_paths"));
    int headers = header_count(c.get());
    restore_array(c->extra_headers, headers, jget(r, "extra_headers"));
    auto *order = jget(r, "mcp_import_order");
    size_t n = array_size(order);
    check(n <= 4);
    c->n_mcp_import_sources = static_cast<int>(n);
    for (size_t i = 0; i < n; ++i) {
        auto *v = yyjson_arr_get(order, i);
        check(!absent(v));
        restore_number(c->mcp_import_order[i], v);
    }
    restore_document(c->settings, jget(r, "settings"));
    restore_document(c->repo_cfg, jget(r, "repo_cfg"));
    allocation_ok();
    return c;
}

void identity(const tny_ctx *ctx, char hex[65]) {
    secret_buffer storage;
    auto *b = &storage.value;
    jescape(b, ctx->base_url ? ctx->base_url : "");
    jescape(b, ctx->auth_header_name ? ctx->auth_header_name : "");
    jescape(b, ctx->auth_header_prefix ? ctx->auth_header_prefix : "");
    for (char **h = ctx->extra_headers; h && *h; ++h) jescape(b, *h);
    yyjson_doc *configs[] = {ctx->settings, ctx->repo_cfg};
    for (size_t i = 0; i < 2; ++i) {
        if (!configs[i]) {
            jescape(b, "{}");
            continue;
        }
        tny::mutable_document copy(tny::required(yyjson_doc_mut_copy(configs[i], jallocator())));
        if (i == 0) {
            auto *root = yyjson_mut_doc_get_root(copy.get());
            yyjson_mut_obj_remove_key(root, "last_provider");
            yyjson_mut_obj_remove_key(root, "last_backend");
            yyjson_mut_obj_remove_key(root, "models");
        }
        secret_string value(jwrite(copy.get()));
        check(value != nullptr); // CP6 serialization must not become default identity
        jescape(b, value.get());
    }
    uint8_t digest[32];
    check(!buf_oom(b) && sha256(reinterpret_cast<const uint8_t *>(b->data), b->len, digest));
    allocation_ok();
    for (size_t i = 0; i < sizeof digest; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
}

bool public_key(const char *name) {
    for (const auto &f : strings)
        if (!f.private_value && strcmp(name, f.name) == 0) return true;
    for (const auto &f : bools)
        if (strcmp(name, f.name) == 0) return true;
    static const char *const rest[] = {
        "backend",
        "max_extension_iterations",
        "extension_timeout_ms",
        "max_steps",
        "perm_mode",
        "tool_profile",
        "image_input",
        "ws_hash",
        "ssh_port",
        "task_digest",
        "instructions_digest",
        "max_tool_result_bytes",
        "mcp_import_mask",
        "extra_dirs",
        "instruction_paths",
        "mcp_import_order",
        "identity",
    };
    for (const char *key : rest)
        if (strcmp(name, key) == 0) return true;
    return false;
}
void finish_profile(tny_ctx *c) {
    if (!c->provider_name || strcmp(c->provider_name, "grok") != 0 ||
        !strstr(c->base_url ? c->base_url : "", "cli-chat-proxy"))
        return;
    // Remove stale routing even for an explicitly absent/empty saved model.
    // Compact without allocating so every exceptional exit sees a valid array.
    constexpr char prefix[] = "x-grok-model-override: ";
    int n = 0;
    int routing_at = -1;
    for (int i = 0; c->extra_headers && c->extra_headers[i]; ++i) {
        char *h = c->extra_headers[i];
        if (strncmp(h, prefix, sizeof prefix - 1) == 0) {
            if (routing_at < 0) routing_at = n;
            secure_free(h);
        } else c->extra_headers[n++] = h;
    }
    if (c->extra_headers) c->extra_headers[n] = nullptr;
    if (!c->model || !*c->model) return;
    // The C append helper uses (n + 2) in signed arithmetic.
    check(n <= INT_MAX - 2);
    tny::string expected(prefix);
    expected += c->model;
    tny_finish_builtin_profile(c);
    allocation_ok();
    check(header_count(c) == n + 1 && strcmp(c->extra_headers[n], expected.c_str()) == 0);
    // Identity includes header order. Replacing a middle routing header must
    // not reorder unrelated headers or change an otherwise valid identity.
    if (routing_at >= 0 && routing_at < n) {
        char *routing = c->extra_headers[n];
        memmove(c->extra_headers + routing_at + 1, c->extra_headers + routing_at,
                static_cast<size_t>(n - routing_at) * sizeof(char *));
        c->extra_headers[routing_at] = routing;
    }
}
context recover(const tny_ctx *resolved, yyjson_val *saved) {
    allocation_ok();
    check(resolved && resolved->cwd && yyjson_is_obj(saved));
    const char *saved_identity = optional_string(jget(saved, "identity"));
    const char *provider = optional_string(jget(saved, "provider_name"));
    const char *cwd = optional_string(jget(saved, "cwd"));
    check(saved_identity && provider && cwd && strcmp(cwd, resolved->cwd) == 0 &&
          strcmp(provider, tny_provider_name(resolved)) == 0 &&
          resolved->backend == TNY_BK_OPENAI && jget_int(saved, "backend", -1) == TNY_BK_OPENAI &&
          !jget_bool(saved, "no_save", true) && !jget_bool(saved, "library_mode", true) &&
          jget_int(saved, "perm_mode", -1) >= TNY_MODE_ASK &&
          jget_int(saved, "tool_profile", 99) <= TNY_TOOLS_TERMINAL);
    check(jget_int(saved, "perm_mode", 99) <= resolved->perm_mode &&
          jget_int(saved, "tool_profile", -1) >= resolved->tool_profile); // CP6 authority oracle
    auto d = tny::make_document();
    auto *merged = encode(d.get(), resolved, false);
    yyjson_mut_doc_set_root(d.get(), merged);
    size_t i, n;
    yyjson_val *key, *value;
    yyjson_obj_foreach(saved, i, n, key, value) {
        const char *name = optional_string(key);
        check(name && public_key(name));
        check(yyjson_obj_get(saved, name) == value); // reject duplicate authority fields
        check(yyjson_mut_obj_put(merged, tny::required(yyjson_mut_strcpy(d.get(), name)),
                                 tny::required(yyjson_val_mut_copy(d.get(), value))));
    }
    tny::document parsed(tny::required(yyjson_mut_doc_imut_copy(d.get(), jallocator())));
    auto ctx = restore(yyjson_doc_get_root(parsed.get()));
    finish_profile(ctx.get());
    char digest[65];
    identity(ctx.get(), digest);
    check(strcmp(saved_identity, digest) == 0);
    enable_extensions(ctx.get());
    allocation_ok();
    return ctx;
}
} // namespace

yyjson_mut_val *tny_checkpoint_context(yyjson_mut_doc *d, const tny_ctx *c) try {
    return encode(d, c, false);
} catch (...) { return nullptr; }

tny_ctx *tny_checkpoint_context_restore(yyjson_val *r) try {
    auto c = restore(r);
    enable_extensions(c.get());
    return c.release();
} catch (...) { return nullptr; }

yyjson_mut_val *tny_checkpoint_public(yyjson_mut_doc *d, const tny_ctx *c) try {
    auto *r = encode(d, c, true);
    char digest[65];
    identity(c, digest);
    check(yyjson_mut_obj_add_strcpy(d, r, "identity", digest));
    allocation_ok();
    return r;
} catch (...) { return nullptr; }

tny_ctx *tny_checkpoint_recover(tny_ctx *resolved, yyjson_val *saved) try {
    return recover(resolved, saved).release();
} catch (...) { return nullptr; }
