/* Checkpoint C facade exercised against the complete injected C/C++ graph. */
#include "core/checkpoint.h"
#include "core/backend.h"
#include "core/extensions.h"
#include "core/runtime.h"
#include "core/perm.h"
#include "core/session.h"
#include "util/alloc.h"
#include "util/util.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static char fixture_dir[] = "/tmp/tny-checkpoint-XXXXXX";

#define REQUIRE(expr)                                                                  \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            fprintf(stderr, "checkpoint assertion at line %d: %s\n", __LINE__, #expr); \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)
static void fault_at(size_t index) {
    char value[32];
    snprintf(value, sizeof value, "%zu", index);
    REQUIRE(setenv("TNY_TEST_ALLOC_SCOPE", "checkpoint-owner", 1) == 0);
    REQUIRE(setenv("TNY_TEST_ALLOC_FAIL_AT", value, 1) == 0);
    tny_alloc_scope_begin("checkpoint-owner");
}
static yyjson_doc *parse(const char *text) {
    yyjson_doc *d = jparse(text, strlen(text));
    REQUIRE(d);
    return d;
}
static void set_string(char **p, const char *text) {
    secure_free(*p);
    *p = text ? tny_alloc_strdup(text) : NULL;
    REQUIRE(!text || *p);
}
static char **array(const char *a, const char *b, const char *c) {
    char **v = tny_alloc_calloc(4, sizeof *v);
    REQUIRE(v);
    v[0] = tny_alloc_strdup(a);
    v[1] = tny_alloc_strdup(b);
    v[2] = tny_alloc_strdup(c);
    REQUIRE(v[0] && v[1] && v[2]);
    return v;
}
static tny_ctx *fixture(bool full) {
    tny_ctx *c = tny_ctx_new_explicit(".", fixture_dir);
    REQUIRE(c);
    c->library_mode = false;
    c->perm_mode = TNY_MODE_YOLO;
    c->tool_profile = TNY_TOOLS_ALL;
    if (!full) return c;
    set_string(&c->provider_name, "grok");
    set_string(&c->model, "fixture-model");
    set_string(&c->base_url, "https://cli-chat-proxy.fixture.invalid/v1");
    set_string(&c->api_key, "SECRET-api_key");
    set_string(&c->auth_header_name, "SECRET-auth_header_name");
    set_string(&c->auth_header_prefix, "SECRET-auth_header_prefix");
    set_string(&c->max_tokens_field, "fixture-max_tokens_field");
    set_string(&c->wire_api, "fixture-wire_api");
    set_string(&c->output_schema, "fixture-output_schema");
    set_string(&c->xai_api_key, "SECRET-xai_api_key");
    set_string(&c->chatgpt_token, "SECRET-chatgpt_token");
    set_string(&c->chatgpt_account_id, "SECRET-chatgpt_account_id");
    set_string(&c->codex_base_url, "SECRET-codex_base_url");
    set_string(&c->ssh_host, "fixture-ssh_host");
    set_string(&c->ssh_cwd, "fixture-ssh_cwd");
    set_string(&c->ssh_control, "fixture-ssh_control");
    set_string(&c->service_tier, "fixture-service_tier");
    set_string(&c->system_prompt, "fixture-system_prompt");
    set_string(&c->task_name, "fixture-task_name");
    set_string(&c->task_source, "fixture-task_source");
    set_string(&c->task_instructions, "fixture-task_instructions");
    set_string(&c->reasoning_effort, "fixture-reasoning_effort");
    set_string(&c->instructions_snapshot, "fixture-instructions_snapshot");
    set_string(&c->sandbox_mode, "fixture-sandbox_mode");
    set_string(&c->settings_path, "fixture-settings_path");
    char long_value[8193];
    memset(long_value, 'z', sizeof long_value - 1);
    long_value[8192] = 0;
    set_string(&c->task_instructions, long_value);
    set_string(&c->instructions_snapshot, long_value);
    snprintf(c->ws_hash, sizeof c->ws_hash, "0123456789abcdef");
    snprintf(c->ssh_port, sizeof c->ssh_port, "2222");
    snprintf(c->task_digest, sizeof c->task_digest, "0123456789012345678901234567890123456789");
    snprintf(c->instructions_digest, sizeof c->instructions_digest, "fedcba9876543210");
    c->model_from_flag = true;
    c->json_out = true;
    c->no_save = false;
    c->no_self_improve = true;
    c->no_color = true;
    c->force_color = true;
    c->library_mode = false;
    c->prompt_optimisation = true;
    c->extensions_enabled = true;
    c->service_tier_explicit = true;
    c->service_tier_from_settings = true;
    c->task_explicit = true;
    c->effort_explicit = true;
    c->effort_from_settings = true;
    c->context_enabled = true;
    c->workspace_read_only = true;
    c->instructions_snapshot_ready = true;
    c->mcp_disabled = true;
    c->mcp_import_warned = true;
    c->backend = TNY_BK_OPENAI;
    c->max_extension_iterations = 17;
    c->extension_timeout_ms = 3210;
    c->max_steps = 73;
    c->image_input = TNY_IMAGE_INPUT_CONFIGURED_UNSUPPORTED;
    c->max_tool_result_bytes = 123456;
    c->mcp_import_mask = 15;
    c->n_mcp_import_sources = 4;
    c->mcp_import_order[0] = 8;
    c->mcp_import_order[1] = 2;
    c->mcp_import_order[2] = 4;
    c->mcp_import_order[3] = 1;
    c->extra_dirs = array("/one", "", "/three");
    c->n_extra_dirs = 3;
    for (int i = 0; i < c->n_instruction_paths; ++i) free(c->instruction_paths[i]);
    free(c->instruction_paths);
    c->instruction_paths = array("/instructions/one", "", "/instructions/three");
    c->n_instruction_paths = 3;
    c->extra_headers =
        array("X-Fixture: SECRET-header", "X-Second: value", "X-XAI-Token-Auth: xai-grok-cli");
    c->settings = parse("{\"secret\":\"SECRET-settings\",\"nested\":{\"a\":[1,true,null]},"
                        "\"models\":{\"grok\":\"old\"}}");
    c->repo_cfg = parse("{\"secret\":\"SECRET-repo\",\"context\":false}");
    tny_finish_builtin_profile(c);
    return c;
}
static yyjson_doc *snapshot(const tny_ctx *c, bool public_only) {
    yyjson_mut_doc *m = yyjson_mut_doc_new(jallocator());
    REQUIRE(m);
    yyjson_mut_val *r = public_only ? tny_checkpoint_public(m, c) : tny_checkpoint_context(m, c);
    REQUIRE(r);
    yyjson_mut_doc_set_root(m, r);
    yyjson_doc *d = yyjson_mut_doc_imut_copy(m, jallocator());
    REQUIRE(d);
    yyjson_mut_doc_free(m);
    return d;
}
static void same(const tny_ctx *c, yyjson_doc *expected) {
    yyjson_doc *d = snapshot(c, false);
    REQUIRE(yyjson_equals(yyjson_doc_get_root(d), yyjson_doc_get_root(expected)));
    yyjson_doc_free(d);
}
/* Independent schema oracle: paired omission in encoder and decoder must fail. */
static void complete_schema(tny_ctx *c) {
    yyjson_doc *d = snapshot(c, false);
    yyjson_val *r = yyjson_doc_get_root(d);
    REQUIRE(yyjson_obj_size(r) == 65);
    REQUIRE(jget_str(r, "cwd") && !strcmp(jget_str(r, "cwd"), c->cwd));
    REQUIRE(jget_str(r, "provider_name") &&
            !strcmp(jget_str(r, "provider_name"), c->provider_name));
    REQUIRE(jget_str(r, "model") && !strcmp(jget_str(r, "model"), c->model));
    REQUIRE(jget_str(r, "base_url") && !strcmp(jget_str(r, "base_url"), c->base_url));
    REQUIRE(jget_str(r, "api_key") && !strcmp(jget_str(r, "api_key"), c->api_key));
    REQUIRE(jget_str(r, "auth_header_name") &&
            !strcmp(jget_str(r, "auth_header_name"), c->auth_header_name));
    REQUIRE(jget_str(r, "auth_header_prefix") &&
            !strcmp(jget_str(r, "auth_header_prefix"), c->auth_header_prefix));
    REQUIRE(jget_str(r, "max_tokens_field") &&
            !strcmp(jget_str(r, "max_tokens_field"), c->max_tokens_field));
    REQUIRE(jget_str(r, "wire_api") && !strcmp(jget_str(r, "wire_api"), c->wire_api));
    REQUIRE(jget_str(r, "output_schema") &&
            !strcmp(jget_str(r, "output_schema"), c->output_schema));
    REQUIRE(jget_str(r, "xai_api_key") && !strcmp(jget_str(r, "xai_api_key"), c->xai_api_key));
    REQUIRE(jget_str(r, "chatgpt_token") &&
            !strcmp(jget_str(r, "chatgpt_token"), c->chatgpt_token));
    REQUIRE(jget_str(r, "chatgpt_account_id") &&
            !strcmp(jget_str(r, "chatgpt_account_id"), c->chatgpt_account_id));
    REQUIRE(jget_str(r, "codex_base_url") &&
            !strcmp(jget_str(r, "codex_base_url"), c->codex_base_url));
    REQUIRE(jget_str(r, "ssh_host") && !strcmp(jget_str(r, "ssh_host"), c->ssh_host));
    REQUIRE(jget_str(r, "ssh_cwd") && !strcmp(jget_str(r, "ssh_cwd"), c->ssh_cwd));
    REQUIRE(jget_str(r, "ssh_control") && !strcmp(jget_str(r, "ssh_control"), c->ssh_control));
    REQUIRE(jget_str(r, "service_tier") && !strcmp(jget_str(r, "service_tier"), c->service_tier));
    REQUIRE(jget_str(r, "system_prompt") &&
            !strcmp(jget_str(r, "system_prompt"), c->system_prompt));
    REQUIRE(jget_str(r, "task_name") && !strcmp(jget_str(r, "task_name"), c->task_name));
    REQUIRE(jget_str(r, "task_source") && !strcmp(jget_str(r, "task_source"), c->task_source));
    REQUIRE(jget_str(r, "task_instructions") &&
            !strcmp(jget_str(r, "task_instructions"), c->task_instructions));
    REQUIRE(jget_str(r, "reasoning_effort") &&
            !strcmp(jget_str(r, "reasoning_effort"), c->reasoning_effort));
    REQUIRE(jget_str(r, "instructions_snapshot") &&
            !strcmp(jget_str(r, "instructions_snapshot"), c->instructions_snapshot));
    REQUIRE(jget_str(r, "sandbox_mode") && !strcmp(jget_str(r, "sandbox_mode"), c->sandbox_mode));
    REQUIRE(jget_str(r, "tny_dir") && !strcmp(jget_str(r, "tny_dir"), c->tny_dir));
    REQUIRE(jget_str(r, "settings_path") &&
            !strcmp(jget_str(r, "settings_path"), c->settings_path));
    REQUIRE(jget_str(r, "ws_hash") && !strcmp(jget_str(r, "ws_hash"), c->ws_hash));
    REQUIRE(jget_str(r, "ssh_port") && !strcmp(jget_str(r, "ssh_port"), c->ssh_port));
    REQUIRE(jget_str(r, "task_digest") && !strcmp(jget_str(r, "task_digest"), c->task_digest));
    REQUIRE(jget_str(r, "instructions_digest") &&
            !strcmp(jget_str(r, "instructions_digest"), c->instructions_digest));
    REQUIRE(yyjson_is_bool(jget(r, "model_from_flag")) &&
            yyjson_get_bool(jget(r, "model_from_flag")) == c->model_from_flag);
    REQUIRE(yyjson_is_bool(jget(r, "json_out")) &&
            yyjson_get_bool(jget(r, "json_out")) == c->json_out);
    REQUIRE(yyjson_is_bool(jget(r, "no_save")) &&
            yyjson_get_bool(jget(r, "no_save")) == c->no_save);
    REQUIRE(yyjson_is_bool(jget(r, "no_self_improve")) &&
            yyjson_get_bool(jget(r, "no_self_improve")) == c->no_self_improve);
    REQUIRE(yyjson_is_bool(jget(r, "no_color")) &&
            yyjson_get_bool(jget(r, "no_color")) == c->no_color);
    REQUIRE(yyjson_is_bool(jget(r, "force_color")) &&
            yyjson_get_bool(jget(r, "force_color")) == c->force_color);
    REQUIRE(yyjson_is_bool(jget(r, "library_mode")) &&
            yyjson_get_bool(jget(r, "library_mode")) == c->library_mode);
    REQUIRE(yyjson_is_bool(jget(r, "prompt_optimisation")) &&
            yyjson_get_bool(jget(r, "prompt_optimisation")) == c->prompt_optimisation);
    REQUIRE(yyjson_is_bool(jget(r, "extensions_enabled")) &&
            yyjson_get_bool(jget(r, "extensions_enabled")) == c->extensions_enabled);
    REQUIRE(yyjson_is_bool(jget(r, "service_tier_explicit")) &&
            yyjson_get_bool(jget(r, "service_tier_explicit")) == c->service_tier_explicit);
    REQUIRE(yyjson_is_bool(jget(r, "service_tier_from_settings")) &&
            yyjson_get_bool(jget(r, "service_tier_from_settings")) ==
                c->service_tier_from_settings);
    REQUIRE(yyjson_is_bool(jget(r, "task_explicit")) &&
            yyjson_get_bool(jget(r, "task_explicit")) == c->task_explicit);
    REQUIRE(yyjson_is_bool(jget(r, "effort_explicit")) &&
            yyjson_get_bool(jget(r, "effort_explicit")) == c->effort_explicit);
    REQUIRE(yyjson_is_bool(jget(r, "effort_from_settings")) &&
            yyjson_get_bool(jget(r, "effort_from_settings")) == c->effort_from_settings);
    REQUIRE(yyjson_is_bool(jget(r, "context_enabled")) &&
            yyjson_get_bool(jget(r, "context_enabled")) == c->context_enabled);
    REQUIRE(yyjson_is_bool(jget(r, "workspace_read_only")) &&
            yyjson_get_bool(jget(r, "workspace_read_only")) == c->workspace_read_only);
    REQUIRE(yyjson_is_bool(jget(r, "instructions_snapshot_ready")) &&
            yyjson_get_bool(jget(r, "instructions_snapshot_ready")) ==
                c->instructions_snapshot_ready);
    REQUIRE(yyjson_is_bool(jget(r, "mcp_disabled")) &&
            yyjson_get_bool(jget(r, "mcp_disabled")) == c->mcp_disabled);
    REQUIRE(yyjson_is_bool(jget(r, "mcp_import_warned")) &&
            yyjson_get_bool(jget(r, "mcp_import_warned")) == c->mcp_import_warned);
    REQUIRE(yyjson_is_int(jget(r, "backend")) &&
            yyjson_get_uint(jget(r, "backend")) == (uint64_t)c->backend);
    REQUIRE(yyjson_is_int(jget(r, "max_extension_iterations")) &&
            yyjson_get_uint(jget(r, "max_extension_iterations")) ==
                (uint64_t)c->max_extension_iterations);
    REQUIRE(yyjson_is_int(jget(r, "extension_timeout_ms")) &&
            yyjson_get_uint(jget(r, "extension_timeout_ms")) == (uint64_t)c->extension_timeout_ms);
    REQUIRE(yyjson_is_int(jget(r, "max_steps")) &&
            yyjson_get_uint(jget(r, "max_steps")) == (uint64_t)c->max_steps);
    REQUIRE(yyjson_is_int(jget(r, "perm_mode")) &&
            yyjson_get_uint(jget(r, "perm_mode")) == (uint64_t)c->perm_mode);
    REQUIRE(yyjson_is_int(jget(r, "tool_profile")) &&
            yyjson_get_uint(jget(r, "tool_profile")) == (uint64_t)c->tool_profile);
    REQUIRE(yyjson_is_int(jget(r, "image_input")) &&
            yyjson_get_uint(jget(r, "image_input")) == (uint64_t)c->image_input);
    REQUIRE(yyjson_is_int(jget(r, "max_tool_result_bytes")) &&
            yyjson_get_uint(jget(r, "max_tool_result_bytes")) ==
                (uint64_t)c->max_tool_result_bytes);
    REQUIRE(yyjson_is_int(jget(r, "mcp_import_mask")) &&
            yyjson_get_uint(jget(r, "mcp_import_mask")) == (uint64_t)c->mcp_import_mask);
    REQUIRE(yyjson_equals(jget(r, "settings"), yyjson_doc_get_root(c->settings)));
    REQUIRE(yyjson_equals(jget(r, "repo_cfg"), yyjson_doc_get_root(c->repo_cfg)));
    REQUIRE(yyjson_arr_size(jget(r, "extra_dirs")) == (size_t)c->n_extra_dirs);
    for (int i = 0; i < c->n_extra_dirs; ++i)
        REQUIRE(!strcmp(yyjson_get_str(yyjson_arr_get(jget(r, "extra_dirs"), (size_t)i)),
                        c->extra_dirs[i]));
    REQUIRE(yyjson_arr_size(jget(r, "instruction_paths")) == (size_t)c->n_instruction_paths);
    for (int i = 0; i < c->n_instruction_paths; ++i)
        REQUIRE(!strcmp(yyjson_get_str(yyjson_arr_get(jget(r, "instruction_paths"), (size_t)i)),
                        c->instruction_paths[i]));
    REQUIRE(yyjson_arr_size(jget(r, "extra_headers")) == (size_t)4);
    for (int i = 0; i < 4; ++i)
        REQUIRE(!strcmp(yyjson_get_str(yyjson_arr_get(jget(r, "extra_headers"), (size_t)i)),
                        c->extra_headers[i]));
    REQUIRE(yyjson_arr_size(jget(r, "mcp_import_order")) == 4);
    for (size_t i = 0; i < 4; ++i)
        REQUIRE(yyjson_get_uint(yyjson_arr_get(jget(r, "mcp_import_order"), i)) ==
                c->mcp_import_order[i]);
    yyjson_doc_free(d);
    d = snapshot(c, true);
    REQUIRE(yyjson_obj_size(yyjson_doc_get_root(d)) == 55);
    yyjson_doc_free(d);
}
static void encoder_lifetime(void) {
    tny_ctx *c = fixture(true);
    yyjson_doc *expected = snapshot(c, false);
    yyjson_mut_doc *d = yyjson_mut_doc_new(jallocator());
    REQUIRE(d);
    yyjson_mut_val *r = tny_checkpoint_context(d, c);
    REQUIRE(r);
    yyjson_mut_doc_set_root(d, r);
    tny_ctx_free(c);
    yyjson_doc *copy = yyjson_mut_doc_imut_copy(d, jallocator());
    REQUIRE(copy);
    REQUIRE(yyjson_equals(yyjson_doc_get_root(copy), yyjson_doc_get_root(expected)));
    yyjson_doc_free(copy);
    yyjson_doc_free(expected);
    yyjson_mut_doc_free(d);
}
static void roundtrip(tny_ctx *c) {
    yyjson_doc *expected = snapshot(c, false);
    yyjson_doc *input = snapshot(c, false);
    tny_ctx *restored = tny_checkpoint_context_restore(yyjson_doc_get_root(input));
    REQUIRE(restored);
    yyjson_doc_free(input);
    same(restored, expected);
    REQUIRE(restored->cwd != c->cwd);
    REQUIRE(restored->no_self_improve == c->no_self_improve);
    REQUIRE(!c->settings || restored->settings != c->settings);
    REQUIRE(!c->extensions_enabled || restored->extensions);
    if (c->extensions_enabled) {
        REQUIRE(tny_extensions_entry_count(restored->extensions) == 2);
        REQUIRE(tny_extensions_get_state(restored->extensions) == TNY_EXTENSIONS_DORMANT);
    }
    tny_ctx_free(restored);
    yyjson_doc_free(expected);
}
static yyjson_doc *changed(yyjson_doc *input, const char *name, const char *json) {
    yyjson_mut_doc *m = yyjson_doc_mut_copy(input, jallocator());
    REQUIRE(m);
    yyjson_mut_val *r = yyjson_mut_doc_get_root(m);
    if (json) {
        yyjson_doc *v = parse(json);
        REQUIRE(yyjson_mut_obj_put(r, yyjson_mut_strcpy(m, name),
                                   yyjson_val_mut_copy(m, yyjson_doc_get_root(v))));
        yyjson_doc_free(v);
    } else yyjson_mut_obj_remove_key(r, name);
    yyjson_doc *d = yyjson_mut_doc_imut_copy(m, jallocator());
    REQUIRE(d);
    yyjson_mut_doc_free(m);
    return d;
}
static void invalid(yyjson_doc *input, const char *name, const char *json) {
    yyjson_doc *d = changed(input, name, json);
    tny_ctx *c = tny_checkpoint_context_restore(yyjson_doc_get_root(d));
    REQUIRE(!c);
    yyjson_doc_free(d);
}
static void reject(tny_ctx *c, yyjson_doc *input, const char *name, const char *json) {
    yyjson_doc *d = changed(input, name, json);
    yyjson_doc *contents = snapshot(c, false);
    tny_ctx before;
    memcpy(&before, c, sizeof before);
    REQUIRE(!tny_checkpoint_recover(c, yyjson_doc_get_root(d)));
    REQUIRE(memcmp(&before, c, sizeof before) == 0);
    same(c, contents);
    yyjson_doc_free(contents);
    yyjson_doc_free(d);
}
static void edge_cases(tny_ctx *full) {
    yyjson_doc *d = snapshot(full, false);
    const char *string_keys[] = {
        "cwd",
        "provider_name",
        "model",
        "base_url",
        "api_key",
        "auth_header_name",
        "auth_header_prefix",
        "max_tokens_field",
        "wire_api",
        "output_schema",
        "xai_api_key",
        "chatgpt_token",
        "chatgpt_account_id",
        "codex_base_url",
        "ssh_host",
        "ssh_cwd",
        "ssh_control",
        "service_tier",
        "system_prompt",
        "task_name",
        "task_source",
        "task_instructions",
        "reasoning_effort",
        "instructions_snapshot",
        "sandbox_mode",
        "tny_dir",
        "settings_path",
    };
    for (size_t i = 0; i < sizeof string_keys / sizeof string_keys[0]; ++i) {
        const char *key = string_keys[i];
        invalid(d, key, "42");
        invalid(d, key, "\"embedded\\u0000suffix\"");
        if (!strcmp(key, "cwd") || !strcmp(key, "tny_dir")) continue;
        for (int j = 0; j < 2; ++j) {
            yyjson_doc *input = changed(d, key, j ? "null" : NULL);
            tny_ctx *c = tny_checkpoint_context_restore(yyjson_doc_get_root(input));
            REQUIRE(c);
            yyjson_doc_free(input);
            yyjson_doc *actual = snapshot(c, false);
            REQUIRE(!jget(yyjson_doc_get_root(actual), key));
            yyjson_doc_free(actual);
            tny_ctx_free(c);
        }
    }
    invalid(d, "extra_dirs", "[\"one\",null]");
    invalid(d, "instruction_paths", "[\"one\",{}]");
    invalid(d, "extra_headers", "[\"one\",17]");
    invalid(d, "extra_headers", "{}");
    invalid(d, "mcp_import_order", "[1,2,4,8,16]");
    invalid(d, "mcp_import_order", "[1,-1]");
    invalid(d, "mcp_import_order", "[null]");
    invalid(d, "mcp_import_mask", "4294967296");
    invalid(d, "max_steps", "2147483648");
    invalid(d, "max_steps", "1.5");
    invalid(d, "max_tool_result_bytes", "-1");
    invalid(d, "perm_mode", "99");
    invalid(d, "tool_profile", "-1");
    invalid(d, "image_input", "3");
    invalid(d, "no_save", "\"false\"");
    invalid(d, "ssh_port", "\"123456\"");
    invalid(d, "ws_hash", "\"12345678901234567\"");
    invalid(d, "instructions_digest", "\"12345678901234567\"");
    invalid(d, "task_digest", "\"12345678901234567890123456789012345678901\"");
    yyjson_doc_free(d);
    d = snapshot(full, true);
    char *encoded = jwrite_val(yyjson_doc_get_root(d));
    REQUIRE(encoded);
    REQUIRE(!strstr(encoded, "SECRET-"));
    secure_free(encoded);
    const char *private_keys[] = {
        "api_key",       "base_url",      "auth_header_name",   "auth_header_prefix",
        "extra_headers", "chatgpt_token", "chatgpt_account_id", "codex_base_url",
        "xai_api_key",   "settings",      "repo_cfg",           "unknown"};
    for (size_t i = 0; i < sizeof private_keys / sizeof private_keys[0]; ++i)
        reject(full, d, private_keys[i], "null");
    reject(full, d, "identity", "\"mismatch\"");
    reject(full, d, "identity", NULL);
    reject(full, d, "identity", "null");
    reject(full, d, "backend", "1");
    reject(full, d, "backend", "2");
    reject(full, d, "backend", "-1");
    reject(full, d, "backend", "null");
    full->backend = -1; /* An unresolved context cannot recover a native checkpoint. */
    reject(full, d, "backend", "0");
    full->backend = TNY_BK_OPENAI;
    reject(full, d, "provider_name", "\"other\"");
    reject(full, d, "cwd", "\"/elsewhere\"");
    reject(full, d, "no_save", "true");
    reject(full, d, "library_mode", "true");
    reject(full, d, "perm_mode", "null");
    reject(full, d, "tool_profile", "1.5");
    reject(full, d, "workspace_read_only", "false");
    reject(full, d, "workspace_read_only", "null");
    full->perm_mode = TNY_MODE_ASK;
    reject(full, d, "perm_mode", "2");
    full->perm_mode = TNY_MODE_YOLO;
    full->tool_profile = TNY_TOOLS_TERMINAL;
    reject(full, d, "tool_profile", "0");
    full->tool_profile = TNY_TOOLS_ALL;
    for (int missing = 0; missing < 2; ++missing) {
        yyjson_doc *input = changed(d, "system_prompt", missing ? NULL : "null");
        tny_ctx *c = tny_checkpoint_recover(full, yyjson_doc_get_root(input));
        REQUIRE(c);
        yyjson_doc_free(input);
        if (missing) REQUIRE(c->system_prompt && !strcmp(c->system_prompt, full->system_prompt));
        else REQUIRE(!c->system_prompt);
        tny_ctx_free(c);
    }
    yyjson_mut_doc *duplicate = yyjson_doc_mut_copy(d, jallocator());
    REQUIRE(duplicate);
    REQUIRE(yyjson_mut_obj_add_int(duplicate, yyjson_mut_doc_get_root(duplicate), "perm_mode", 0));
    yyjson_doc *duplicated = yyjson_mut_doc_imut_copy(duplicate, jallocator());
    REQUIRE(duplicated);
    REQUIRE(!tny_checkpoint_recover(full, yyjson_doc_get_root(duplicated)));
    yyjson_doc_free(duplicated);
    yyjson_mut_doc_free(duplicate);
    yyjson_doc_free(d);
    // Empty/absent/null arrays and null JSON documents retain their schema meanings.
    yyjson_doc *empty = parse("{\"cwd\":\".\",\"tny_dir\":\"/"
                              "nonexistent-tny-checkpoint-fixture\",\"extra_dirs\":null,"
                              "\"settings\":null,\"repo_cfg\":null}");
    tny_ctx *c = tny_checkpoint_context_restore(yyjson_doc_get_root(empty));
    REQUIRE(c);
    yyjson_doc_free(empty);
    REQUIRE(c->n_extra_dirs == 0 && c->extra_dirs && !c->extra_dirs[0]);
    REQUIRE(c->n_instruction_paths == 0 && c->instruction_paths && !c->instruction_paths[0]);
    REQUIRE(c->extra_headers && !c->extra_headers[0]);
    REQUIRE(c->settings && yyjson_is_null(yyjson_doc_get_root(c->settings)));
    REQUIRE(c->max_tool_result_bytes == 32768);
    roundtrip(c);
    tny_ctx_free(c);
}
static void backend_bounds(tny_ctx *c) {
    const int original = c->backend;
    for (int backend = -1; backend < TNY_BK_COUNT; ++backend) {
        c->backend = backend;
        roundtrip(c);
    }
    c->backend = original;
    yyjson_doc *d = snapshot(c, false);
    invalid(d, "backend", "-2");
    char out_of_range[32];
    snprintf(out_of_range, sizeof out_of_range, "%d", TNY_BK_COUNT);
    invalid(d, "backend", out_of_range);
    yyjson_doc_free(d);
}

static void sweep(tny_ctx *c, int operation) {
    yyjson_doc *before = snapshot(c, false);
    yyjson_doc *input = snapshot(c, operation == 3);
    tny_ctx bytes;
    memcpy(&bytes, c, sizeof bytes);
    size_t count = 0;
    for (size_t index = 0;; ++index) {
        yyjson_mut_doc *out = NULL;
        tny_ctx *result = NULL;
        bool ok;
        fault_at(index);
        if (operation == 0 || operation == 2) {
            out = yyjson_mut_doc_new(jallocator());
            yyjson_mut_val *r = out ? (operation == 0 ? tny_checkpoint_context(out, c)
                                                      : tny_checkpoint_public(out, c))
                                    : NULL;
            ok = r != NULL;
        } else {
            result = operation == 1 ? tny_checkpoint_context_restore(yyjson_doc_get_root(input))
                                    : tny_checkpoint_recover(c, yyjson_doc_get_root(input));
            ok = result != NULL;
        }
        if (index == 0) {
            count = tny_alloc_test_scope_count();
            REQUIRE(ok && count > 0);
        } else {
            REQUIRE(tny_alloc_test_scope_injected());
            REQUIRE(!ok);
        }
        REQUIRE(memcmp(&bytes, c, sizeof bytes) == 0);
        fault_at(0);
        same(c, before);
        if (result) {
            same(result, before);
            REQUIRE(!c->extensions_enabled || result->extensions);
        }
        tny_ctx_free(result);
        yyjson_mut_doc_free(out);
        if (index == count) break;
    }
    printf("checkpoint operation %d: %zu discovered faults rejected\n", operation, count);
    yyjson_doc_free(input);
    yyjson_doc_free(before);
}
static void recovery_routing(tny_ctx *saved) {
    yyjson_doc *public = snapshot(saved, true);
    tny_ctx *resolved = fixture(true);
    set_string(&resolved->model, "new-default-model");
    set_string(&resolved->api_key, "SECRET-refreshed-credential");
    tny_ctx_clear_extra_headers(resolved);
    resolved->extra_headers =
        array("X-Fixture: SECRET-header", "X-Second: value", "X-XAI-Token-Auth: xai-grok-cli");
    tny_finish_builtin_profile(resolved);
    yyjson_doc *before = snapshot(resolved, false);
    tny_ctx bytes;
    memcpy(&bytes, resolved, sizeof bytes);
    size_t count = 0;
    for (size_t index = 0;; ++index) {
        fault_at(index);
        tny_ctx *c = tny_checkpoint_recover(resolved, yyjson_doc_get_root(public));
        if (!index) {
            count = tny_alloc_test_scope_count();
            REQUIRE(c && count);
        } else {
            REQUIRE(tny_alloc_test_scope_injected());
            REQUIRE(!c);
        }
        REQUIRE(memcmp(&bytes, resolved, sizeof bytes) == 0);
        fault_at(0);
        same(resolved, before);
        if (c) {
            REQUIRE(!strcmp(c->model, saved->model));
            REQUIRE(!strcmp(c->api_key, resolved->api_key));
            REQUIRE(c->api_key != resolved->api_key);
            REQUIRE(!strcmp(c->extra_headers[3], "x-grok-model-override: fixture-model"));
            REQUIRE(!c->extra_headers[4]);
        }
        tny_ctx_free(c);
        if (index == count) break;
    }
    printf("checkpoint saved-model recovery: %zu faults, caller unchanged\n", count);
    yyjson_doc_free(before);
    yyjson_doc_free(public);
    tny_ctx_free(resolved);
}
/* These shapes reproduced the independent review's routing bug. A routing
 * header may be absent, duplicated by the resolved profile, or followed by
 * unrelated headers. Every failure must preserve the resolved context. */
static void routing_edges(void) {
    const char *models[] = {NULL, "", "fixture-model"};
    for (size_t shape = 0; shape < sizeof models / sizeof models[0]; ++shape) {
        tny_ctx *saved = fixture(true), *resolved = fixture(true);
        set_string(&saved->model, models[shape]);
        set_string(&resolved->model, "new-default-model");
        tny_ctx_clear_extra_headers(saved);
        tny_ctx_clear_extra_headers(resolved);
        saved->extra_headers =
            array("X-Fixture: SECRET-header", "X-Second: value", "X-XAI-Token-Auth: xai-grok-cli");
        resolved->extra_headers =
            array("X-Fixture: SECRET-header", "X-Second: value", "X-XAI-Token-Auth: xai-grok-cli");
        tny_finish_builtin_profile(saved);
        tny_finish_builtin_profile(resolved);
        if (shape == 2) {
            tny_ctx *contexts[] = {saved, resolved};
            for (size_t i = 0; i < sizeof contexts / sizeof contexts[0]; ++i) {
                char **headers = contexts[i]->extra_headers;
                char *routing = headers[3];
                headers[3] = headers[2];
                headers[2] = headers[1];
                headers[1] = routing;
            }
        }
        tny_ctx_add_extra_header(resolved, "x-grok-model-override: duplicate-stale-model");
        REQUIRE(resolved->extra_headers[4] && !resolved->extra_headers[5]);
        yyjson_doc *public = snapshot(saved, true), *expected = snapshot(saved, false);
        yyjson_doc *before = snapshot(resolved, false);
        tny_ctx bytes;
        memcpy(&bytes, resolved, sizeof bytes);
        size_t count = 0;
        for (size_t index = 0;; ++index) {
            fault_at(index);
            tny_ctx *result = tny_checkpoint_recover(resolved, yyjson_doc_get_root(public));
            if (index == 0) {
                count = tny_alloc_test_scope_count();
                REQUIRE(result && count);
            } else {
                REQUIRE(tny_alloc_test_scope_injected());
                REQUIRE(!result);
            }
            REQUIRE(memcmp(&bytes, resolved, sizeof bytes) == 0);
            fault_at(0);
            same(resolved, before);
            if (result) same(result, expected);
            tny_ctx_free(result);
            if (index == count) break;
        }
        printf("checkpoint routing shape %zu: %zu faults, caller unchanged\n", shape, count);
        yyjson_doc_free(public);
        yyjson_doc_free(expected);
        yyjson_doc_free(before);
        tny_ctx_free(saved);
        tny_ctx_free(resolved);
    }
}
/* The runner's failed restart continues through after_backend(), which
 * invokes this same allocation-free engine settlement. Its thread-local
 * failure latch must not poison the next checkpoint operation. */
static void checkpoint_retry_after_settlement(void) {
    tny_ctx *c = fixture(false);
    c->no_save = true;
    tny_session_state *session = session_new(c);
    REQUIRE(session);
    perm_engine *perm = perm_new(c);
    REQUIRE(perm);
    tny_engine *engine = tny_engine_new(c, session, perm, NULL, NULL);
    REQUIRE(engine);
    yyjson_mut_doc *d = yyjson_mut_doc_new(jallocator());
    REQUIRE(d);
    fault_at(1);
    REQUIRE(!tny_checkpoint_context(d, c));
    REQUIRE(tny_alloc_test_scope_injected() && tny_alloc_scope_failed());
    const size_t allocations = tny_alloc_test_scope_count();
    tny_engine_fail_oom(engine);
    REQUIRE(!tny_alloc_scope_failed());
    REQUIRE(tny_alloc_test_scope_count() == allocations);
    REQUIRE(tny_alloc_test_settlement_allocations() == 0);
    /* No test reset/begin/clear between the failure and the retry. */
    REQUIRE(tny_checkpoint_context(d, c));
    yyjson_mut_doc_free(d);
    tny_engine_free(engine);
    perm_free(perm);
    session_close(session);
    tny_ctx_free(c);
    fault_at(0);
    puts("checkpoint retry: engine settlement clears latch without allocating");
}
static void bench(void) {
    tny_ctx *c = fixture(true);
    const int rounds = 2000;
    clock_t start = clock();
    for (int i = 0; i < rounds; ++i) {
        yyjson_doc *d = snapshot(c, false);
        tny_ctx *copy = tny_checkpoint_context_restore(yyjson_doc_get_root(d));
        REQUIRE(copy);
        tny_ctx_free(copy);
        yyjson_doc_free(d);
    }
    printf("checkpoint private roundtrip: %d cycles %.6f cpu seconds\n", rounds,
           (double)(clock() - start) / CLOCKS_PER_SEC);
    tny_ctx_free(c);
}
static void fixture_file(const char *relative) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", fixture_dir, relative);
    FILE *file = fopen(path, "w");
    REQUIRE(file);
    REQUIRE(fputs("# inert checkpoint fixture\n", file) >= 0);
    REQUIRE(fclose(file) == 0);
}
static void setup(void) {
    REQUIRE(mkdtemp(fixture_dir));
    char path[512];
    const char *dirs[] = {"extensions", "extensions/package", "extensions/collision"};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        snprintf(path, sizeof path, "%s/%s", fixture_dir, dirs[i]);
        REQUIRE(mkdir(path, 0700) == 0);
    }
    fixture_file("extensions/simple.py");
    fixture_file("extensions/package/index.py");
    fixture_file("extensions/collision.py");
    fixture_file("extensions/collision/index.py");
}
static void cleanup(void) {
    const char *files[] = {"extensions/simple.py", "extensions/package/index.py",
                           "extensions/collision.py", "extensions/collision/index.py"};
    const char *dirs[] = {"extensions/package", "extensions/collision", "extensions"};
    char path[512];
    for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
        snprintf(path, sizeof path, "%s/%s", fixture_dir, files[i]);
        REQUIRE(unlink(path) == 0);
    }
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        snprintf(path, sizeof path, "%s/%s", fixture_dir, dirs[i]);
        REQUIRE(rmdir(path) == 0);
    }
    REQUIRE(rmdir(fixture_dir) == 0);
}
int main(int argc, char **argv) {
    setup();
    fault_at(0);
    if (argc == 2 && !strcmp(argv[1], "--bench")) {
        bench();
        cleanup();
        return 0;
    }
    tny_ctx *full = fixture(true), *empty = fixture(false);
    checkpoint_retry_after_settlement();
    complete_schema(full);
    backend_bounds(full);
    encoder_lifetime();
    roundtrip(full);
    roundtrip(empty);
    edge_cases(full);
    for (int operation = 0; operation < 4; ++operation) {
        sweep(full, operation);
        sweep(empty, operation);
    }
    recovery_routing(full);
    routing_edges();
    for (int i = 0; i < 32; ++i) {
        roundtrip(full);
        roundtrip(empty);
    }
    // A failed enclosing API scope must never be cleared by any checkpoint entry.
    yyjson_doc *private = snapshot(full, false), *public = snapshot(full, true);
    yyjson_mut_doc *out = yyjson_mut_doc_new(jallocator());
    REQUIRE(out);
    fault_at(1);
    REQUIRE(!tny_alloc_malloc(1));
    REQUIRE(!tny_checkpoint_context(out, full));
    REQUIRE(!tny_checkpoint_public(out, full));
    REQUIRE(!tny_checkpoint_context_restore(yyjson_doc_get_root(private)));
    REQUIRE(!tny_checkpoint_recover(full, yyjson_doc_get_root(public)));
    REQUIRE(tny_alloc_scope_failed());
    fault_at(0);
    yyjson_doc_free(private);
    yyjson_doc_free(public);
    yyjson_mut_doc_free(out);
    out = yyjson_mut_doc_new(jallocator());
    REQUIRE(out);
    full->n_mcp_import_sources = 5;
    REQUIRE(!tny_checkpoint_context(out, full));
    full->n_mcp_import_sources = 4;
    full->n_extra_dirs = -1;
    REQUIRE(!tny_checkpoint_context(out, full));
    full->n_extra_dirs = 3;
    yyjson_mut_doc_free(out);
    tny_ctx_free(full);
    tny_ctx_free(empty);
    REQUIRE(tny_alloc_test_owned_live() == 0);
    cleanup();
    puts("checkpoint ownership: PASS");
    return 0;
}
