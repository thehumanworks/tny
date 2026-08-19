#include "core/config.h"
#include "core/backend.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

const char *tny_perm_mode_name(tny_perm_mode m) {
    switch (m) {
    case TNY_MODE_AUTO: return "auto";
    case TNY_MODE_YOLO: return "yolo";
    default: return "ask";
    }
}

static const char *bk_names[TNY_BK_COUNT] = {"openai", "cursor", "codex", "acp"};

const char *tny_backend_name(tny_backend_id id) {
    return (id >= 0 && id < TNY_BK_COUNT) ? bk_names[id] : "unknown";
}

int tny_backend_from_name(const char *name) {
    for (int i = 0; i < TNY_BK_COUNT; i++)
        if (strcmp(name, bk_names[i]) == 0) return i;
    return -1;
}

static char *dup_or(const char *env, const char *dflt) {
    const char *v = getenv(env);
    return xstrdup(v && *v ? v : dflt);
}

/* settings.json object for this workspace: settings.workspaces[cwd] */
static yyjson_val *ws_obj(tny_ctx *ctx) {
    if (!ctx->settings) return NULL;
    yyjson_val *root = yyjson_doc_get_root(ctx->settings);
    yyjson_val *all = jget(root, "workspaces");
    return jget(all, ctx->cwd);
}

tny_ctx *tny_ctx_load(const char *cwd_flag) {
    tny_ctx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return NULL;

    if (cwd_flag) {
        ctx->cwd = path_abs(cwd_flag);
        if (!ctx->cwd || !dir_exists(ctx->cwd)) {
            fprintf(stderr, "tny: --cwd %s: not a directory\n", cwd_flag);
            free(ctx->cwd);
            free(ctx);
            return NULL;
        }
    } else {
        char tmp[PATH_MAX];
        if (!getcwd(tmp, sizeof tmp)) { free(ctx); return NULL; }
        ctx->cwd = xstrdup(tmp);
    }
    snprintf(ctx->ws_hash, sizeof ctx->ws_hash, "%016llx",
             (unsigned long long)fnv1a(ctx->cwd, strlen(ctx->cwd)));

    ctx->tny_dir = path_tny_dir();
    ctx->settings_path = path_join(ctx->tny_dir, "settings.json");
    ctx->settings = jparse_file(ctx->settings_path);

    char *repo_cfg_path = path_join(ctx->cwd, ".tny.json");
    ctx->repo_cfg = jparse_file(repo_cfg_path);
    free(repo_cfg_path);

    yyjson_val *sroot = ctx->settings ? yyjson_doc_get_root(ctx->settings) : NULL;
    yyjson_val *wso = ws_obj(ctx);
    yyjson_val *rroot = ctx->repo_cfg ? yyjson_doc_get_root(ctx->repo_cfg) : NULL;

    /* defaults */
    ctx->backend = -1;
    ctx->perm_mode = TNY_MODE_ASK;
    ctx->max_steps = 24;
    ctx->max_tool_result_bytes = 32768;
    ctx->context_enabled = true;
    ctx->sandbox_mode = xstrdup("auto");

    /* settings-level. Models are per-provider ("models" object, applied in
     * tny_resolve_backend) — a global model would leak one provider's id
     * into another's thread/start. */
    const char *s;
    if ((s = jget_str(wso, "permission_mode")) || (s = jget_str(sroot, "permission_mode"))) {
        if (strcmp(s, "auto") == 0) ctx->perm_mode = TNY_MODE_AUTO;
        else if (strcmp(s, "yolo") == 0) ctx->perm_mode = TNY_MODE_YOLO;
    }
    const char *pm_env = getenv("TNY_PERMISSION_MODE");
    if (pm_env) {
        if (strcmp(pm_env, "auto") == 0) ctx->perm_mode = TNY_MODE_AUTO;
        else if (strcmp(pm_env, "yolo") == 0) ctx->perm_mode = TNY_MODE_YOLO;
        else if (strcmp(pm_env, "ask") == 0) ctx->perm_mode = TNY_MODE_ASK;
    }

    /* openai provider: settings "openai" object, then env */
    yyjson_val *oa = jget(sroot, "openai");
    const char *bu = getenv("OPENAI_BASE_URL");
    if (!bu || !*bu) bu = jget_str(oa, "base_url");
    ctx->base_url = xstrdup(bu && *bu ? bu : "https://api.openai.com/v1");
    const char *key_env = jget_str(oa, "api_key_env");
    const char *key = key_env ? getenv(key_env) : NULL;
    if (!key || !*key) key = getenv("OPENAI_API_KEY");
    ctx->api_key = key && *key ? xstrdup(key) : NULL;
    const char *ahn = jget_str(oa, "auth_header_name");
    const char *ahp = jget_str(oa, "auth_header_prefix");
    ctx->auth_header_name = xstrdup(ahn ? ahn : "Authorization");
    ctx->auth_header_prefix = xstrdup(ahp ? ahp : "Bearer ");
    const char *mtf = jget_str(oa, "max_tokens_field");
    ctx->max_tokens_field = mtf ? xstrdup(mtf) : NULL;

    /* host backend knobs */
    ctx->bridge_bin = dup_or("CURSOR_SDK_BRIDGE_BIN", "cursor-sdk-bridge");
    ctx->codex_ws = NULL; /* set by flag; default handled by backend */
    ctx->codex_bin = dup_or("TNY_CODEX_BIN", "codex");
    const char *tf = getenv("CODEX_REMOTE_TOKEN");
    ctx->ws_token_file = NULL;
    (void)tf; /* env token consumed directly by the codex backend */

    /* repo limits — never authority */
    if (rroot) {
        ctx->max_steps = (int)jget_int(rroot, "steps", ctx->max_steps);
        ctx->max_tool_result_bytes =
            (size_t)jget_int(rroot, "max_tool_result_bytes", (int64_t)ctx->max_tool_result_bytes);
        ctx->context_enabled = jget_bool(rroot, "context", ctx->context_enabled);
        const char *sb = jget_str(rroot, "sandbox");
        if (sb) { free(ctx->sandbox_mode); ctx->sandbox_mode = xstrdup(sb); }
    }

    /* saved extra dirs for this workspace */
    yyjson_val *dirs = jget(wso, "additional_dirs");
    if (dirs && yyjson_is_arr(dirs)) {
        size_t idx, max;
        yyjson_val *v;
        yyjson_arr_foreach(dirs, idx, max, v) {
            if (!yyjson_is_str(v)) continue;
            ctx->extra_dirs = realloc(ctx->extra_dirs,
                                      sizeof(char *) * (size_t)(ctx->n_extra_dirs + 1));
            ctx->extra_dirs[ctx->n_extra_dirs++] = xstrdup(yyjson_get_str(v));
        }
    }
    return ctx;
}

/* A `codex login` drops auth.json under $CODEX_HOME (default ~/.codex).
 * Its presence means the user's ChatGPT subscription can drive the codex
 * backend with no API key at all. */
bool tny_codex_auth_present(void) {
    char *dir;
    const char *ch = getenv("CODEX_HOME");
    if (ch && *ch) {
        dir = xstrdup(ch);
    } else {
        char *home = path_home();
        if (!home) return false;
        dir = path_join(home, ".codex");
        free(home);
    }
    char *p = path_join(dir, "auth.json");
    free(dir);
    bool ok = file_exists(p);
    free(p);
    return ok;
}

const char *tny_settings_provider_model(tny_ctx *ctx, const char *provider) {
    if (!ctx->settings) return NULL;
    return jget_str(jget(yyjson_doc_get_root(ctx->settings), "models"), provider);
}

/* Once the provider is known, pick its model: --model beats the saved
 * per-provider entry beats the openai object's model (openai only). */
static void apply_provider_model(tny_ctx *ctx, int id) {
    if (ctx->model_from_flag) return;
    const char *m = tny_settings_provider_model(ctx, tny_backend_name(id));
    if (!m && id == TNY_BK_OPENAI && ctx->settings)
        m = jget_str(jget(yyjson_doc_get_root(ctx->settings), "openai"), "model");
    if (m && *m) {
        free(ctx->model);
        ctx->model = xstrdup(m);
    }
}

int tny_resolve_backend(tny_ctx *ctx, const char *flag_value) {
    int id = -1;
    if (flag_value) {
        id = tny_backend_from_name(flag_value);
        if (id < 0) {
            fprintf(stderr, "tny: unknown provider '%s' (cursor|codex|acp|openai)\n",
                    flag_value);
            return -1;
        }
    }
    if (id < 0) { /* the provider (and model) last used wins over detection */
        const char *last = tny_settings_get_str(ctx, "last_provider");
        if (!last) last = tny_settings_get_str(ctx, "last_backend");
        if (last) id = tny_backend_from_name(last);
    }
    if (id < 0) {
        const char *e1 = getenv("OPENAI_BASE_URL"), *e2 = getenv("OPENAI_API_KEY");
        if ((e1 && *e1) || (e2 && *e2)) id = TNY_BK_OPENAI;
    }
    /* No explicit choice anywhere: prefer subscription logins over raw keys
     * (docs/cli.md "Provider selection"). Codex login first, then a Cursor
     * key from the environment, then the openai backend's own error path. */
    if (id < 0 && tny_codex_auth_present()) id = TNY_BK_CODEX;
    if (id < 0) {
        const char *ck = getenv("CURSOR_API_KEY");
        if (ck && *ck) id = TNY_BK_CURSOR;
    }
    if (id < 0) id = TNY_BK_OPENAI;
    ctx->backend = id;
    apply_provider_model(ctx, id);
    return ctx->backend;
}

const char *tny_settings_get_str(tny_ctx *ctx, const char *key) {
    if (!ctx->settings) return NULL;
    return jget_str(yyjson_doc_get_root(ctx->settings), key);
}

/* Rewrite settings.json applying fn(mut_root). */
typedef void (*settings_edit_fn)(yyjson_mut_doc *doc, yyjson_mut_val *root, void *ud);

static int settings_edit(tny_ctx *ctx, settings_edit_fn fn, void *ud) {
    yyjson_mut_doc *doc;
    if (ctx->settings) {
        doc = yyjson_doc_mut_copy(ctx->settings, NULL);
    } else {
        doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_doc_set_root(doc, yyjson_mut_obj(doc));
    }
    if (!doc) return -1;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    fn(doc, root, ud);
    char *out = jwrite_pretty(doc);
    yyjson_mut_doc_free(doc);
    if (!out) return -1;
    mkdir_p(ctx->tny_dir);
    int rc = file_write_atomic(ctx->settings_path, out, strlen(out));
    free(out);
    if (rc == 0) {
        yyjson_doc_free(ctx->settings);
        ctx->settings = jparse_file(ctx->settings_path);
    }
    return rc;
}

struct kv { const char *k, *v; };

static void edit_set_str(yyjson_mut_doc *doc, yyjson_mut_val *root, void *ud) {
    struct kv *kv = ud;
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, kv->k), yyjson_mut_strcpy(doc, kv->v));
}

int tny_settings_set_str(tny_ctx *ctx, const char *key, const char *value) {
    struct kv kv = {key, value};
    return settings_edit(ctx, edit_set_str, &kv);
}

static void edit_remember_use(yyjson_mut_doc *doc, yyjson_mut_val *root, void *ud) {
    struct kv *kv = ud; /* k = provider name, v = model or NULL */
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, "last_provider"),
                       yyjson_mut_strcpy(doc, kv->k));
    if (!kv->v) return;
    yyjson_mut_val *models = yyjson_mut_obj_get(root, "models");
    if (!models || !yyjson_mut_is_obj(models)) {
        models = yyjson_mut_obj(doc);
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, "models"), models);
    }
    yyjson_mut_obj_put(models, yyjson_mut_strcpy(doc, kv->k),
                       yyjson_mut_strcpy(doc, kv->v));
}

int tny_settings_remember_use(tny_ctx *ctx) {
    const char *name = tny_backend_name(ctx->backend);
    const char *last = tny_settings_get_str(ctx, "last_provider");
    const char *saved = tny_settings_provider_model(ctx, name);
    bool same_last = last && strcmp(last, name) == 0;
    bool same_model = ctx->model ? (saved && strcmp(saved, ctx->model) == 0) : true;
    if (same_last && same_model) return 0; /* nothing new to write */
    struct kv kv = {name, ctx->model};
    return settings_edit(ctx, edit_remember_use, &kv);
}

struct ws_edit { tny_ctx *ctx; const char *dir; int op; }; /* 0 add, 1 remove, 2 clear */

static yyjson_mut_val *ws_mut_obj(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *cwd) {
    yyjson_mut_val *all = yyjson_mut_obj_get(root, "workspaces");
    if (!all) {
        all = yyjson_mut_obj(doc);
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, "workspaces"), all);
    }
    yyjson_mut_val *ws = yyjson_mut_obj_get(all, cwd);
    if (!ws) {
        ws = yyjson_mut_obj(doc);
        yyjson_mut_obj_put(all, yyjson_mut_strcpy(doc, cwd), ws);
    }
    return ws;
}

static void edit_ws(yyjson_mut_doc *doc, yyjson_mut_val *root, void *ud) {
    struct ws_edit *e = ud;
    yyjson_mut_val *ws = ws_mut_obj(doc, root, e->ctx->cwd);
    yyjson_mut_val *arr = yyjson_mut_obj_get(ws, "additional_dirs");
    if (e->op == 2) {
        yyjson_mut_obj_put(ws, yyjson_mut_strcpy(doc, "additional_dirs"), yyjson_mut_arr(doc));
        return;
    }
    yyjson_mut_val *narr = yyjson_mut_arr(doc);
    if (arr && yyjson_mut_is_arr(arr)) {
        size_t idx, max;
        yyjson_mut_val *v;
        yyjson_mut_arr_foreach(arr, idx, max, v) {
            const char *s = yyjson_mut_get_str(v);
            if (!s) continue;
            if (e->op == 1 && strcmp(s, e->dir) == 0) continue;
            if (e->op == 0 && strcmp(s, e->dir) == 0) continue; /* dedupe, re-added below */
            yyjson_mut_arr_add_val(narr, yyjson_mut_strcpy(doc, s));
        }
    }
    if (e->op == 0) yyjson_mut_arr_add_val(narr, yyjson_mut_strcpy(doc, e->dir));
    yyjson_mut_obj_put(ws, yyjson_mut_strcpy(doc, "additional_dirs"), narr);
}

int tny_workspace_add(tny_ctx *ctx, const char *dir) {
    char *abs = path_abs(dir);
    if (!abs || !dir_exists(abs)) {
        fprintf(stderr, "tny: %s: not a directory\n", dir);
        free(abs);
        return -1;
    }
    struct ws_edit e = {ctx, abs, 0};
    int rc = settings_edit(ctx, edit_ws, &e);
    free(abs);
    return rc;
}

int tny_workspace_remove(tny_ctx *ctx, const char *dir) {
    char *abs = path_abs(dir);
    struct ws_edit e = {ctx, abs ? abs : dir, 1};
    int rc = settings_edit(ctx, edit_ws, &e);
    free(abs);
    return rc;
}

int tny_workspace_clear(tny_ctx *ctx) {
    struct ws_edit e = {ctx, NULL, 2};
    return settings_edit(ctx, edit_ws, &e);
}

void tny_ctx_free(tny_ctx *ctx) {
    if (!ctx) return;
    free(ctx->cwd);
    for (int i = 0; i < ctx->n_extra_dirs; i++) free(ctx->extra_dirs[i]);
    free(ctx->extra_dirs);
    free(ctx->model);
    free(ctx->base_url);
    free(ctx->api_key);
    free(ctx->auth_header_name);
    free(ctx->auth_header_prefix);
    free(ctx->max_tokens_field);
    free(ctx->bridge_bin);
    free(ctx->codex_ws);
    free(ctx->codex_bin);
    free(ctx->ws_token_file);
    free(ctx->service_tier);
    if (ctx->agent_argv) {
        for (char **p = ctx->agent_argv; *p; p++) free(*p);
        free(ctx->agent_argv);
    }
    free(ctx->sandbox_mode);
    free(ctx->tny_dir);
    free(ctx->settings_path);
    yyjson_doc_free(ctx->settings);
    yyjson_doc_free(ctx->repo_cfg);
    free(ctx);
}
