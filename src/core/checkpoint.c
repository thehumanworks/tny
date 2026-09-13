#include "core/checkpoint.h"
#include "core/extensions.h"
#include "util/util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

yyjson_mut_val *tny_checkpoint_context(yyjson_mut_doc *d, const tny_ctx *c) {
    yyjson_mut_val *r = yyjson_mut_obj(d);
    if (c->cwd) yyjson_mut_obj_add_strcpy(d, r, "cwd", c->cwd);
    if (c->provider_name) yyjson_mut_obj_add_strcpy(d, r, "provider_name", c->provider_name);
    if (c->model) yyjson_mut_obj_add_strcpy(d, r, "model", c->model);
    if (c->base_url) yyjson_mut_obj_add_strcpy(d, r, "base_url", c->base_url);
    if (c->api_key) yyjson_mut_obj_add_strcpy(d, r, "api_key", c->api_key);
    if (c->auth_header_name)
        yyjson_mut_obj_add_strcpy(d, r, "auth_header_name", c->auth_header_name);
    if (c->auth_header_prefix)
        yyjson_mut_obj_add_strcpy(d, r, "auth_header_prefix", c->auth_header_prefix);
    if (c->max_tokens_field)
        yyjson_mut_obj_add_strcpy(d, r, "max_tokens_field", c->max_tokens_field);
    if (c->wire_api) yyjson_mut_obj_add_strcpy(d, r, "wire_api", c->wire_api);
    if (c->output_schema) yyjson_mut_obj_add_strcpy(d, r, "output_schema", c->output_schema);
    if (c->bridge_bin) yyjson_mut_obj_add_strcpy(d, r, "bridge_bin", c->bridge_bin);
    if (c->xai_api_key) yyjson_mut_obj_add_strcpy(d, r, "xai_api_key", c->xai_api_key);
    if (c->chatgpt_token) yyjson_mut_obj_add_strcpy(d, r, "chatgpt_token", c->chatgpt_token);
    if (c->chatgpt_account_id)
        yyjson_mut_obj_add_strcpy(d, r, "chatgpt_account_id", c->chatgpt_account_id);
    if (c->codex_base_url) yyjson_mut_obj_add_strcpy(d, r, "codex_base_url", c->codex_base_url);
    if (c->ssh_host) yyjson_mut_obj_add_strcpy(d, r, "ssh_host", c->ssh_host);
    if (c->ssh_cwd) yyjson_mut_obj_add_strcpy(d, r, "ssh_cwd", c->ssh_cwd);
    if (c->ssh_control) yyjson_mut_obj_add_strcpy(d, r, "ssh_control", c->ssh_control);
    if (c->service_tier) yyjson_mut_obj_add_strcpy(d, r, "service_tier", c->service_tier);
    if (c->system_prompt) yyjson_mut_obj_add_strcpy(d, r, "system_prompt", c->system_prompt);
    if (c->task_name) yyjson_mut_obj_add_strcpy(d, r, "task_name", c->task_name);
    if (c->task_source) yyjson_mut_obj_add_strcpy(d, r, "task_source", c->task_source);
    if (c->task_instructions)
        yyjson_mut_obj_add_strcpy(d, r, "task_instructions", c->task_instructions);
    if (c->reasoning_effort)
        yyjson_mut_obj_add_strcpy(d, r, "reasoning_effort", c->reasoning_effort);
    if (c->instructions_snapshot)
        yyjson_mut_obj_add_strcpy(d, r, "instructions_snapshot", c->instructions_snapshot);
    if (c->sandbox_mode) yyjson_mut_obj_add_strcpy(d, r, "sandbox_mode", c->sandbox_mode);
    if (c->tny_dir) yyjson_mut_obj_add_strcpy(d, r, "tny_dir", c->tny_dir);
    if (c->settings_path) yyjson_mut_obj_add_strcpy(d, r, "settings_path", c->settings_path);
    yyjson_mut_obj_add_strcpy(d, r, "ws_hash", c->ws_hash);
    yyjson_mut_obj_add_strcpy(d, r, "ssh_port", c->ssh_port);
    yyjson_mut_obj_add_strcpy(d, r, "task_digest", c->task_digest);
    yyjson_mut_obj_add_strcpy(d, r, "instructions_digest", c->instructions_digest);
    yyjson_mut_obj_add_bool(d, r, "model_from_flag", c->model_from_flag);
    yyjson_mut_obj_add_bool(d, r, "json_out", c->json_out);
    yyjson_mut_obj_add_bool(d, r, "no_save", c->no_save);
    yyjson_mut_obj_add_bool(d, r, "no_color", c->no_color);
    yyjson_mut_obj_add_bool(d, r, "force_color", c->force_color);
    yyjson_mut_obj_add_bool(d, r, "library_mode", c->library_mode);
    yyjson_mut_obj_add_bool(d, r, "prompt_optimisation", c->prompt_optimisation);
    yyjson_mut_obj_add_bool(d, r, "extensions_enabled", c->extensions_enabled);
    yyjson_mut_obj_add_bool(d, r, "no_host_registry", c->no_host_registry);
    yyjson_mut_obj_add_bool(d, r, "service_tier_explicit", c->service_tier_explicit);
    yyjson_mut_obj_add_bool(d, r, "service_tier_from_settings", c->service_tier_from_settings);
    yyjson_mut_obj_add_bool(d, r, "task_explicit", c->task_explicit);
    yyjson_mut_obj_add_bool(d, r, "effort_explicit", c->effort_explicit);
    yyjson_mut_obj_add_bool(d, r, "effort_from_settings", c->effort_from_settings);
    yyjson_mut_obj_add_bool(d, r, "context_enabled", c->context_enabled);
    yyjson_mut_obj_add_bool(d, r, "instructions_snapshot_ready", c->instructions_snapshot_ready);
    yyjson_mut_obj_add_bool(d, r, "mcp_disabled", c->mcp_disabled);
    yyjson_mut_obj_add_bool(d, r, "mcp_import_warned", c->mcp_import_warned);
    yyjson_mut_obj_add_int(d, r, "backend", c->backend);
    yyjson_mut_obj_add_int(d, r, "max_extension_iterations", c->max_extension_iterations);
    yyjson_mut_obj_add_int(d, r, "extension_timeout_ms", c->extension_timeout_ms);
    yyjson_mut_obj_add_int(d, r, "max_steps", c->max_steps);
    yyjson_mut_obj_add_int(d, r, "perm_mode", c->perm_mode);
    yyjson_mut_obj_add_int(d, r, "tool_profile", c->tool_profile);
    yyjson_mut_obj_add_int(d, r, "image_input", c->image_input);
    yyjson_mut_obj_add_uint(d, r, "max_tool_result_bytes", c->max_tool_result_bytes);
    yyjson_mut_obj_add_uint(d, r, "mcp_import_mask", c->mcp_import_mask);
    yyjson_mut_val *extra_dirs = yyjson_mut_arr(d);
    for (int i = 0; i < c->n_extra_dirs; i++)
        yyjson_mut_arr_add_strcpy(d, extra_dirs, c->extra_dirs[i]);
    yyjson_mut_obj_add_val(d, r, "extra_dirs", extra_dirs);
    yyjson_mut_val *instruction_paths = yyjson_mut_arr(d);
    for (int i = 0; i < c->n_instruction_paths; i++)
        yyjson_mut_arr_add_strcpy(d, instruction_paths, c->instruction_paths[i]);
    yyjson_mut_obj_add_val(d, r, "instruction_paths", instruction_paths);
    yyjson_mut_val *extra_headers = yyjson_mut_arr(d);
    for (int i = 0; c->extra_headers && c->extra_headers[i]; i++)
        yyjson_mut_arr_add_strcpy(d, extra_headers, c->extra_headers[i]);
    yyjson_mut_obj_add_val(d, r, "extra_headers", extra_headers);
    yyjson_mut_val *order = yyjson_mut_arr(d);
    for (int i = 0; i < c->n_mcp_import_sources; i++)
        yyjson_mut_arr_add_uint(d, order, c->mcp_import_order[i]);
    yyjson_mut_obj_add_val(d, r, "mcp_import_order", order);
    if (c->settings)
        yyjson_mut_obj_add_val(d, r, "settings",
                               yyjson_val_mut_copy(d, yyjson_doc_get_root(c->settings)));
    if (c->repo_cfg)
        yyjson_mut_obj_add_val(d, r, "repo_cfg",
                               yyjson_val_mut_copy(d, yyjson_doc_get_root(c->repo_cfg)));
    return r;
}

static yyjson_doc *copy_doc(yyjson_val *value) {
    char *json = yyjson_val_write(value, 0, NULL);
    yyjson_doc *d = json ? jparse(json, strlen(json)) : NULL;
    free(json);
    return d;
}

tny_ctx *tny_checkpoint_context_restore(yyjson_val *r) {
    const char *cwd = jget_str(r, "cwd"), *dir = jget_str(r, "tny_dir");
    if (!cwd || !dir) return NULL;
    tny_ctx *c = tny_ctx_new_explicit(cwd, dir);
    if (!c) return NULL;
    free(c->cwd);
    c->cwd = jget_str(r, "cwd") ? xstrdup(jget_str(r, "cwd")) : NULL;
    if (jget_str(r, "cwd") && !c->cwd) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->provider_name);
    c->provider_name = jget_str(r, "provider_name") ? xstrdup(jget_str(r, "provider_name")) : NULL;
    if (jget_str(r, "provider_name") && !c->provider_name) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->model);
    c->model = jget_str(r, "model") ? xstrdup(jget_str(r, "model")) : NULL;
    if (jget_str(r, "model") && !c->model) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->base_url);
    c->base_url = jget_str(r, "base_url") ? xstrdup(jget_str(r, "base_url")) : NULL;
    if (jget_str(r, "base_url") && !c->base_url) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->api_key);
    c->api_key = jget_str(r, "api_key") ? xstrdup(jget_str(r, "api_key")) : NULL;
    if (jget_str(r, "api_key") && !c->api_key) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->auth_header_name);
    c->auth_header_name =
        jget_str(r, "auth_header_name") ? xstrdup(jget_str(r, "auth_header_name")) : NULL;
    free(c->auth_header_prefix);
    c->auth_header_prefix =
        jget_str(r, "auth_header_prefix") ? xstrdup(jget_str(r, "auth_header_prefix")) : NULL;
    free(c->max_tokens_field);
    c->max_tokens_field =
        jget_str(r, "max_tokens_field") ? xstrdup(jget_str(r, "max_tokens_field")) : NULL;
    free(c->wire_api);
    c->wire_api = jget_str(r, "wire_api") ? xstrdup(jget_str(r, "wire_api")) : NULL;
    if (jget_str(r, "wire_api") && !c->wire_api) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->output_schema);
    c->output_schema = jget_str(r, "output_schema") ? xstrdup(jget_str(r, "output_schema")) : NULL;
    if (jget_str(r, "output_schema") && !c->output_schema) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->bridge_bin);
    c->bridge_bin = jget_str(r, "bridge_bin") ? xstrdup(jget_str(r, "bridge_bin")) : NULL;
    if (jget_str(r, "bridge_bin") && !c->bridge_bin) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->xai_api_key);
    c->xai_api_key = jget_str(r, "xai_api_key") ? xstrdup(jget_str(r, "xai_api_key")) : NULL;
    if (jget_str(r, "xai_api_key") && !c->xai_api_key) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->chatgpt_token);
    c->chatgpt_token = jget_str(r, "chatgpt_token") ? xstrdup(jget_str(r, "chatgpt_token")) : NULL;
    if (jget_str(r, "chatgpt_token") && !c->chatgpt_token) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->chatgpt_account_id);
    c->chatgpt_account_id =
        jget_str(r, "chatgpt_account_id") ? xstrdup(jget_str(r, "chatgpt_account_id")) : NULL;
    free(c->codex_base_url);
    c->codex_base_url =
        jget_str(r, "codex_base_url") ? xstrdup(jget_str(r, "codex_base_url")) : NULL;
    free(c->ssh_host);
    c->ssh_host = jget_str(r, "ssh_host") ? xstrdup(jget_str(r, "ssh_host")) : NULL;
    if (jget_str(r, "ssh_host") && !c->ssh_host) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->ssh_cwd);
    c->ssh_cwd = jget_str(r, "ssh_cwd") ? xstrdup(jget_str(r, "ssh_cwd")) : NULL;
    if (jget_str(r, "ssh_cwd") && !c->ssh_cwd) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->ssh_control);
    c->ssh_control = jget_str(r, "ssh_control") ? xstrdup(jget_str(r, "ssh_control")) : NULL;
    if (jget_str(r, "ssh_control") && !c->ssh_control) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->service_tier);
    c->service_tier = jget_str(r, "service_tier") ? xstrdup(jget_str(r, "service_tier")) : NULL;
    if (jget_str(r, "service_tier") && !c->service_tier) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->system_prompt);
    c->system_prompt = jget_str(r, "system_prompt") ? xstrdup(jget_str(r, "system_prompt")) : NULL;
    if (jget_str(r, "system_prompt") && !c->system_prompt) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->task_name);
    c->task_name = jget_str(r, "task_name") ? xstrdup(jget_str(r, "task_name")) : NULL;
    if (jget_str(r, "task_name") && !c->task_name) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->task_source);
    c->task_source = jget_str(r, "task_source") ? xstrdup(jget_str(r, "task_source")) : NULL;
    if (jget_str(r, "task_source") && !c->task_source) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->task_instructions);
    c->task_instructions =
        jget_str(r, "task_instructions") ? xstrdup(jget_str(r, "task_instructions")) : NULL;
    free(c->reasoning_effort);
    c->reasoning_effort =
        jget_str(r, "reasoning_effort") ? xstrdup(jget_str(r, "reasoning_effort")) : NULL;
    free(c->instructions_snapshot);
    c->instructions_snapshot =
        jget_str(r, "instructions_snapshot") ? xstrdup(jget_str(r, "instructions_snapshot")) : NULL;
    free(c->sandbox_mode);
    c->sandbox_mode = jget_str(r, "sandbox_mode") ? xstrdup(jget_str(r, "sandbox_mode")) : NULL;
    if (jget_str(r, "sandbox_mode") && !c->sandbox_mode) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->tny_dir);
    c->tny_dir = jget_str(r, "tny_dir") ? xstrdup(jget_str(r, "tny_dir")) : NULL;
    if (jget_str(r, "tny_dir") && !c->tny_dir) {
        tny_ctx_free(c);
        return NULL;
    }
    free(c->settings_path);
    c->settings_path = jget_str(r, "settings_path") ? xstrdup(jget_str(r, "settings_path")) : NULL;
    if (jget_str(r, "settings_path") && !c->settings_path) {
        tny_ctx_free(c);
        return NULL;
    }
    snprintf(c->ws_hash, sizeof c->ws_hash, "%s",
             jget_str(r, "ws_hash") ? jget_str(r, "ws_hash") : "");
    snprintf(c->ssh_port, sizeof c->ssh_port, "%s",
             jget_str(r, "ssh_port") ? jget_str(r, "ssh_port") : "");
    snprintf(c->task_digest, sizeof c->task_digest, "%s",
             jget_str(r, "task_digest") ? jget_str(r, "task_digest") : "");
    snprintf(c->instructions_digest, sizeof c->instructions_digest, "%s",
             jget_str(r, "instructions_digest") ? jget_str(r, "instructions_digest") : "");
    c->model_from_flag = jget_bool(r, "model_from_flag", false);
    c->json_out = jget_bool(r, "json_out", false);
    c->no_save = jget_bool(r, "no_save", false);
    c->no_color = jget_bool(r, "no_color", false);
    c->force_color = jget_bool(r, "force_color", false);
    c->library_mode = jget_bool(r, "library_mode", false);
    c->prompt_optimisation = jget_bool(r, "prompt_optimisation", false);
    c->extensions_enabled = jget_bool(r, "extensions_enabled", false);
    c->no_host_registry = jget_bool(r, "no_host_registry", false);
    c->service_tier_explicit = jget_bool(r, "service_tier_explicit", false);
    c->service_tier_from_settings = jget_bool(r, "service_tier_from_settings", false);
    c->task_explicit = jget_bool(r, "task_explicit", false);
    c->effort_explicit = jget_bool(r, "effort_explicit", false);
    c->effort_from_settings = jget_bool(r, "effort_from_settings", false);
    c->context_enabled = jget_bool(r, "context_enabled", false);
    c->instructions_snapshot_ready = jget_bool(r, "instructions_snapshot_ready", false);
    c->mcp_disabled = jget_bool(r, "mcp_disabled", false);
    c->mcp_import_warned = jget_bool(r, "mcp_import_warned", false);
    c->backend = (int)jget_int(r, "backend", 0);
    c->max_extension_iterations = (int)jget_int(r, "max_extension_iterations", 0);
    c->extension_timeout_ms = (int)jget_int(r, "extension_timeout_ms", 0);
    c->max_steps = (int)jget_int(r, "max_steps", 0);
    c->perm_mode = (tny_perm_mode)jget_int(r, "perm_mode", 0);
    c->tool_profile = (tny_tool_profile)jget_int(r, "tool_profile", 0);
    c->image_input = (tny_image_input_policy)jget_int(r, "image_input", 0);
    c->max_tool_result_bytes = (size_t)jget_int(r, "max_tool_result_bytes", 32768);
    c->mcp_import_mask = (unsigned)jget_int(r, "mcp_import_mask", 0);
    yyjson_val *extra_dirs = jget(r, "extra_dirs");
    size_t n_extra_dirs = yyjson_arr_size(extra_dirs);
    c->extra_dirs = calloc(n_extra_dirs + 1, sizeof(char *));
    if (!c->extra_dirs) {
        tny_ctx_free(c);
        return NULL;
    }
    for (size_t i = 0; i < n_extra_dirs; i++) {
        const char *v = yyjson_get_str(yyjson_arr_get(extra_dirs, i));
        if (!v) {
            tny_ctx_free(c);
            return NULL;
        }
        c->extra_dirs[i] = xstrdup(v);
        if (!c->extra_dirs[i]) {
            tny_ctx_free(c);
            return NULL;
        }
        c->n_extra_dirs++;
    }
    for (int i = 0; i < c->n_instruction_paths; i++) free(c->instruction_paths[i]);
    free(c->instruction_paths);
    c->n_instruction_paths = 0;
    yyjson_val *instruction_paths = jget(r, "instruction_paths");
    size_t n_instruction_paths = yyjson_arr_size(instruction_paths);
    c->instruction_paths = calloc(n_instruction_paths + 1, sizeof(char *));
    if (!c->instruction_paths) {
        tny_ctx_free(c);
        return NULL;
    }
    for (size_t i = 0; i < n_instruction_paths; i++) {
        const char *v = yyjson_get_str(yyjson_arr_get(instruction_paths, i));
        if (!v) {
            tny_ctx_free(c);
            return NULL;
        }
        c->instruction_paths[i] = xstrdup(v);
        if (!c->instruction_paths[i]) {
            tny_ctx_free(c);
            return NULL;
        }
        c->n_instruction_paths++;
    }
    yyjson_val *extra_headers = jget(r, "extra_headers");
    size_t n_extra_headers = yyjson_arr_size(extra_headers);
    c->extra_headers = calloc(n_extra_headers + 1, sizeof(char *));
    if (!c->extra_headers) {
        tny_ctx_free(c);
        return NULL;
    }
    for (size_t i = 0; i < n_extra_headers; i++) {
        const char *v = yyjson_get_str(yyjson_arr_get(extra_headers, i));
        if (!v) {
            tny_ctx_free(c);
            return NULL;
        }
        c->extra_headers[i] = xstrdup(v);
        if (!c->extra_headers[i]) {
            tny_ctx_free(c);
            return NULL;
        }
    }
    yyjson_val *order = jget(r, "mcp_import_order");
    c->n_mcp_import_sources = (int)yyjson_arr_size(order);
    if (c->n_mcp_import_sources > 4) {
        tny_ctx_free(c);
        return NULL;
    }
    for (int i = 0; i < c->n_mcp_import_sources; i++)
        c->mcp_import_order[i] = (unsigned)yyjson_get_uint(yyjson_arr_get(order, (size_t)i));
    if (jget(r, "settings")) c->settings = copy_doc(jget(r, "settings"));
    if (jget(r, "repo_cfg")) c->repo_cfg = copy_doc(jget(r, "repo_cfg"));
    if ((jget(r, "settings") && !c->settings) || (jget(r, "repo_cfg") && !c->repo_cfg)) {
        tny_ctx_free(c);
        return NULL;
    }
    if (c->extensions_enabled)
        c->extensions = tny_extensions_new(c->tny_dir, c->cwd, c->extension_timeout_ms);
    return c;
}

/* Recovery stores effective noncredential options, and digests of configuration
 * that must be re-resolved. Credentials and secret-bearing URLs remain IPC-only. */
static bool checkpoint_identity(const tny_ctx *ctx, char hex[65]) {
    buf_t identity;
    buf_init(&identity);
    jescape(&identity, ctx->base_url ? ctx->base_url : "");
    jescape(&identity, ctx->auth_header_name ? ctx->auth_header_name : "");
    jescape(&identity, ctx->auth_header_prefix ? ctx->auth_header_prefix : "");
    for (char **h = ctx->extra_headers; h && *h; h++) jescape(&identity, *h);
    yyjson_doc *configs[] = {ctx->settings, ctx->repo_cfg};
    for (size_t i = 0; i < sizeof configs / sizeof configs[0]; i++) {
        yyjson_mut_doc *copy = configs[i] ? yyjson_doc_mut_copy(configs[i], jallocator()) : NULL;
        if (configs[i] && !copy) {
            buf_free(&identity);
            return false;
        }
        if (copy && i == 0) {
            yyjson_mut_val *root = yyjson_mut_doc_get_root(copy);
            yyjson_mut_obj_remove_key(root, "last_provider");
            yyjson_mut_obj_remove_key(root, "last_backend");
            yyjson_mut_obj_remove_key(root, "models");
        }
        char *value = copy ? jwrite(copy) : NULL;
        jescape(&identity, value ? value : "{}");
        if (value) secure_free(value);
        yyjson_mut_doc_free(copy);
    }
    uint8_t digest[32];
    bool ok = !buf_oom(&identity) && sha256((const uint8_t *)identity.data, identity.len, digest);
    buf_free(&identity);
    if (!ok) return false;
    for (size_t i = 0; i < sizeof digest; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return true;
}

yyjson_mut_val *tny_checkpoint_public(yyjson_mut_doc *d, const tny_ctx *ctx) {
    yyjson_mut_val *public = tny_checkpoint_context(d, ctx);
    yyjson_mut_obj_put(public, yyjson_mut_str(d, "provider_name"),
                       yyjson_mut_strcpy(d, tny_provider_name(ctx)));
    static const char *const private_keys[] = {"api_key",
                                               "base_url",
                                               "auth_header_name",
                                               "auth_header_prefix",
                                               "extra_headers",
                                               "chatgpt_token",
                                               "chatgpt_account_id",
                                               "codex_base_url",
                                               "xai_api_key",
                                               "settings",
                                               "repo_cfg"};
    for (size_t i = 0; i < sizeof private_keys / sizeof private_keys[0]; i++)
        yyjson_mut_obj_remove_key(public, private_keys[i]);
    static const char *const nullable[] = {"cwd",
                                           "provider_name",
                                           "model",
                                           "max_tokens_field",
                                           "wire_api",
                                           "output_schema",
                                           "bridge_bin",
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
                                           "settings_path"};
    for (size_t i = 0; i < sizeof nullable / sizeof nullable[0]; i++)
        if (!yyjson_mut_obj_get(public, nullable[i]))
            yyjson_mut_obj_add_null(d, public, nullable[i]);
    char digest[65];
    if (!checkpoint_identity(ctx, digest)) return NULL;
    yyjson_mut_obj_add_strcpy(d, public, "identity", digest);
    return public;
}

tny_ctx *tny_checkpoint_recover(tny_ctx *resolved, yyjson_val *public) {
    const char *identity = jget_str(public, "identity");
    const char *provider = jget_str(public, "provider_name");
    const char *cwd = jget_str(public, "cwd");
    const char *model = jget_str(public, "model");
    if (!identity || !provider || !cwd || strcmp(cwd, resolved->cwd) != 0 ||
        strcmp(provider, tny_provider_name(resolved)) != 0 || resolved->backend != TNY_BK_OPENAI ||
        jget_int(public, "backend", -1) != TNY_BK_OPENAI || jget_bool(public, "no_save", true) ||
        jget_bool(public, "library_mode", true) ||
        jget_int(public, "perm_mode", -1) < TNY_MODE_ASK ||
        jget_int(public, "tool_profile", 99) > TNY_TOOLS_TERMINAL ||
        jget_int(public, "perm_mode", 99) > resolved->perm_mode ||
        jget_int(public, "tool_profile", -1) < resolved->tool_profile)
        return NULL;
    /* Recompute builtin routing headers with the saved model before comparing
     * identity. Refreshable credentials are still those resolved by the caller. */
    char *old_model = resolved->model;
    resolved->model = model ? xstrdup(model) : NULL;
    tny_finish_builtin_profile(resolved);
    char digest[65];
    bool valid = checkpoint_identity(resolved, digest) && strcmp(identity, digest) == 0;
    free(resolved->model);
    resolved->model = old_model;
    tny_finish_builtin_profile(resolved);
    if (!valid) return NULL;
    yyjson_mut_doc *d = yyjson_mut_doc_new(jallocator());
    if (!d) return NULL;
    yyjson_mut_val *merged = tny_checkpoint_context(d, resolved);
    yyjson_mut_doc_set_root(d, merged);
    /* Only fields produced by our public encoder are accepted. Never let a
     * forged public snapshot replace a credential or its source. */
    yyjson_mut_val *allowed = tny_checkpoint_public(d, resolved);
    size_t i, n;
    yyjson_val *key, *value;
    yyjson_obj_foreach(public, i, n, key, value) {
        const char *name = yyjson_get_str(key);
        if (!yyjson_mut_obj_get(allowed, name)) {
            yyjson_mut_doc_free(d);
            return NULL;
        }
        yyjson_mut_obj_put(merged, yyjson_mut_strcpy(d, name), yyjson_val_mut_copy(d, value));
    }
    char *json = jwrite(d);
    yyjson_doc *parsed = json ? jparse(json, strlen(json)) : NULL;
    tny_ctx *ctx = parsed ? tny_checkpoint_context_restore(yyjson_doc_get_root(parsed)) : NULL;
    yyjson_doc_free(parsed);
    if (json) secure_free(json);
    yyjson_mut_doc_free(d);
    if (ctx) tny_finish_builtin_profile(ctx);
    return ctx;
}
