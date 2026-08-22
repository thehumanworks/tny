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

bool tny_tier_is_fast(const char *tier) {
    return tier && (strcmp(tier, "fast") == 0 || strcmp(tier, "priority") == 0);
}

static const char *bk_names[TNY_BK_COUNT] = {"openai", "cursor", "codex", "acp"};

/* Canonical levels (TNY_EFFORT_LEVELS) and their per-provider wire words.
 * Providers advertise more values than they share ("minimal", "ultra", …);
 * those pass through tny_effort_wire verbatim so the catalog stays usable. */
static const struct {
    const char *level;
    const char *openai; /* chat completions `reasoning_effort` */
    const char *codex;  /* app-server turn/start `effort` */
    const char *cursor; /* ModelSelection params candidate value */
} EFFORTS[] = {
    /* "max" is not an OpenAI chat-completions value: clamp it to xhigh. */
    {"off",    "none",   "none",   "none"},
    {"light",  "low",    "low",    "low"},
    {"medium", "medium", "medium", "medium"},
    {"high",   "high",   "high",   "high"},
    {"xhigh",  "xhigh",  "xhigh",  "xhigh"},
    {"max",    "xhigh",  "max",    "max"},
};
#define N_EFFORTS ((int)(sizeof EFFORTS / sizeof *EFFORTS))

bool tny_effort_canonical(const char *v) {
    if (!v) return false;
    for (int i = 0; i < N_EFFORTS; i++)
        if (strcmp(EFFORTS[i].level, v) == 0) return true;
    return false;
}

const char *tny_effort_wire(int backend, const char *v) {
    if (!v) return NULL;
    for (int i = 0; i < N_EFFORTS; i++) {
        if (strcmp(EFFORTS[i].level, v) != 0) continue;
        switch (backend) {
        case TNY_BK_OPENAI: return EFFORTS[i].openai;
        case TNY_BK_CODEX:  return EFFORTS[i].codex;
        case TNY_BK_CURSOR: return EFFORTS[i].cursor;
        default:            return EFFORTS[i].level;
        }
    }
    return v; /* provider-advertised token: trust the catalog */
}

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

const char *tny_provider_name(const tny_ctx *ctx) {
    return ctx->provider_name ? ctx->provider_name
                              : tny_backend_name((tny_backend_id)ctx->backend);
}

/* Any top-level settings object with a base_url is a user-named
 * OpenAI-compatible provider profile ("openrouter", "xai", …). The base_url
 * requirement keeps reserved objects (workspaces, models, permission) from
 * ever being mistaken for one. */
static yyjson_val *custom_provider_obj(tny_ctx *ctx, const char *name) {
    /* callers exclude builtin names (tny_custom_provider_exists guard) */
    if (!ctx->settings || !name || !*name) return NULL;
    yyjson_val *o = jget(yyjson_doc_get_root(ctx->settings), name);
    if (!yyjson_is_obj(o)) return NULL;
    const char *bu = jget_str(o, "base_url");
    return bu && *bu ? o : NULL;
}

/* NAME + suffix as an env-var name: "openrouter" + "_API_KEY" ->
 * OPENROUTER_API_KEY (uppercased, non-alphanumerics -> '_'). */
char *tny_provider_env_var(const char *name, const char *suffix) {
    size_t n = strlen(name), m = strlen(suffix);
    char *s = malloc(n + m + 1);
    if (!s) return NULL;
    for (size_t i = 0; i != n; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') s[i] = (char)(c - 'a' + 'A');
        else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) s[i] = c;
        else s[i] = '_';
    }
    memcpy(s + n, suffix, m + 1);
    return s;
}

/* Value of the provider's derived env var (NAME_BASE_URL, …), or NULL. */
static const char *derived_env_value(const char *name, const char *suffix) {
    char *var = tny_provider_env_var(name, suffix);
    const char *v = var ? getenv(var) : NULL;
    free(var);
    return v && *v ? v : NULL;
}

/* A provider name is valid when settings has a base_url object for it OR
 * NAME_BASE_URL is set in the environment. Env lookups here are lazy
 * in-memory reads at resolve time — startup paths never run them. */
bool tny_custom_provider_exists(tny_ctx *ctx, const char *name) {
    if (!name || !*name || tny_backend_from_name(name) >= 0) return false;
    if (custom_provider_obj(ctx, name)) return true;
    return derived_env_value(name, "_BASE_URL") != NULL;
}

char *tny_custom_provider_key_env(tny_ctx *ctx, const char *name) {
    if (!tny_custom_provider_exists(ctx, name)) return NULL;
    const char *env = jget_str(custom_provider_obj(ctx, name), "api_key_env");
    return env && *env ? xstrdup(env) : tny_provider_env_var(name, "_API_KEY");
}

extern char **environ;

/* Provider names defined by NAME_BASE_URL environment variables: lowercased
 * prefix, builtins excluded, prefixes outside [A-Z0-9_] skipped (they could
 * not round-trip through tny_provider_env_var). malloc'd array; the
 * caller frees entries and the array. A one-pass in-memory walk of environ
 * (microseconds), run only when a provider is being resolved or listed. */
char **tny_env_provider_names(int *count) {
    static const size_t suf = sizeof "_BASE_URL" - 1;
    char **v = NULL;
    int n = 0;
    for (char **e = environ; e && *e; e++) {
        const char *s = *e;
        const char *eq = strchr(s, '=');
        if (!eq || !eq[1]) continue; /* no value: the provider is not set */
        size_t klen = (size_t)(eq - s);
        if (klen <= suf || memcmp(s + klen - suf, "_BASE_URL", suf) != 0)
            continue;
        size_t plen = klen - suf;
        char *name = malloc(plen + 1);
        if (!name) continue;
        bool ok = true;
        for (size_t i = 0; i != plen; i++) {
            char c = s[i];
            if (c >= 'A' && c <= 'Z') name[i] = (char)(c - 'A' + 'a');
            else if ((c >= '0' && c <= '9') || c == '_') name[i] = c;
            else { ok = false; break; }
        }
        name[plen] = 0;
        if (!ok || tny_backend_from_name(name) >= 0) { free(name); continue; }
        bool dup = false;
        for (int i = 0; i < n; i++)
            if (strcmp(v[i], name) == 0) { dup = true; break; }
        if (dup) { free(name); continue; }
        char **nv = realloc(v, sizeof(char *) * (size_t)(n + 2));
        if (!nv) { free(name); break; }
        v = nv;
        v[n++] = name;
        v[n] = NULL;
    }
    if (count) *count = n;
    return v;
}

/* Auto-detection (no flag, no last_provider): pick an env-defined provider
 * only when exactly one has BOTH NAME_BASE_URL and NAME_API_KEY set — a
 * lone *_BASE_URL from some unrelated tool must never hijack the default.
 * Keyless local gateways still work via an explicit --provider NAME (and
 * last_provider remembers it). Returns a malloc'd name or NULL. */
static char *env_sole_detected_provider(void) {
    int n = 0;
    char **v = tny_env_provider_names(&n);
    char *pick = NULL;
    int hits = 0;
    for (int i = 0; i < n; i++) {
        if (!derived_env_value(v[i], "_API_KEY")) continue;
        hits++;
        if (!pick) pick = xstrdup(v[i]);
    }
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
    if (hits == 1) return pick;
    free(pick);
    return NULL;
}

/* Load the builtin openai profile (settings "openai" object + OPENAI_* env)
 * into ctx. Also used to restore the defaults after a named profile. */
static void load_openai_profile(tny_ctx *ctx) {
    yyjson_val *sroot = ctx->settings ? yyjson_doc_get_root(ctx->settings) : NULL;
    yyjson_val *oa = jget(sroot, "openai");
    const char *bu = getenv("OPENAI_BASE_URL");
    if (!bu || !*bu) bu = jget_str(oa, "base_url");
    free(ctx->base_url);
    ctx->base_url = xstrdup(bu && *bu ? bu : "https://api.openai.com/v1");
    const char *key_env = jget_str(oa, "api_key_env");
    const char *key = key_env ? getenv(key_env) : NULL;
    if (!key || !*key) key = getenv("OPENAI_API_KEY");
    free(ctx->api_key);
    ctx->api_key = key && *key ? xstrdup(key) : NULL;
    const char *ahn = jget_str(oa, "auth_header_name");
    const char *ahp = jget_str(oa, "auth_header_prefix");
    free(ctx->auth_header_name);
    free(ctx->auth_header_prefix);
    ctx->auth_header_name = xstrdup(ahn ? ahn : "Authorization");
    ctx->auth_header_prefix = xstrdup(ahp ? ahp : "Bearer ");
    const char *mtf = jget_str(oa, "max_tokens_field");
    free(ctx->max_tokens_field);
    ctx->max_tokens_field = mtf ? xstrdup(mtf) : NULL;
}

/* Point ctx at a named provider: settings profile, env vars, or both
 * (NAME_BASE_URL beats the profile's base_url, mirroring how
 * OPENAI_BASE_URL beats the "openai" object). The key comes from the
 * profile's own api_key_env (default NAME_API_KEY) — never from
 * OPENAI_API_KEY, which belongs to a different provider. */
static void apply_custom_provider(tny_ctx *ctx, const char *name) {
    yyjson_val *o = custom_provider_obj(ctx, name); /* NULL when env-only */
    free(ctx->provider_name);
    ctx->provider_name = xstrdup(name);
    const char *bu = derived_env_value(name, "_BASE_URL");
    if (!bu) bu = jget_str(o, "base_url");
    free(ctx->base_url);
    ctx->base_url = xstrdup(bu ? bu : "");
    char *key_env = tny_custom_provider_key_env(ctx, name);
    const char *key = key_env ? getenv(key_env) : NULL;
    free(key_env);
    free(ctx->api_key);
    ctx->api_key = key && *key ? xstrdup(key) : NULL;
    const char *ahn = jget_str(o, "auth_header_name");
    const char *ahp = jget_str(o, "auth_header_prefix");
    free(ctx->auth_header_name);
    free(ctx->auth_header_prefix);
    ctx->auth_header_name = xstrdup(ahn ? ahn : "Authorization");
    ctx->auth_header_prefix = xstrdup(ahp ? ahp : "Bearer ");
    const char *mtf = jget_str(o, "max_tokens_field");
    free(ctx->max_tokens_field);
    ctx->max_tokens_field = mtf ? xstrdup(mtf) : NULL;
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

    /* defaults. yolo is deliberate: host providers run their own loops and
     * never hand tny a real approval gate, so tny runs every agent in yolo
     * unless the user explicitly opts into ask/auto (docs/adr/0001). */
    ctx->backend = -1;
    ctx->perm_mode = TNY_MODE_YOLO;
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
        else if (strcmp(s, "ask") == 0) ctx->perm_mode = TNY_MODE_ASK;
    }
    const char *pm_env = getenv("TNY_PERMISSION_MODE");
    if (pm_env) {
        if (strcmp(pm_env, "auto") == 0) ctx->perm_mode = TNY_MODE_AUTO;
        else if (strcmp(pm_env, "yolo") == 0) ctx->perm_mode = TNY_MODE_YOLO;
        else if (strcmp(pm_env, "ask") == 0) ctx->perm_mode = TNY_MODE_ASK;
    }

    /* reasoning effort: env here; a settings.json default is applied after
     * the provider resolves (apply_provider_effort); --effort overrides in
     * cli_make_ctx. tny never *writes* the effort back to settings — a
     * scripted `tny ask --effort X` must not change tomorrow's session
     * (docs/adr/0009, docs/adr/0015). */
    const char *re_env = getenv("TNY_REASONING_EFFORT");
    if (re_env && *re_env) ctx->reasoning_effort = xstrdup(re_env);

    /* openai provider: settings "openai" object, then env. A user-named
     * profile picked in tny_resolve_backend replaces these fields. */
    load_openai_profile(ctx);

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
 * per-provider entry beats the provider object's model (openai-compatible
 * profiles only, builtin "openai" included) beats NAME_DEFAULT_MODEL from
 * the environment (every provider, e.g. CODEX_DEFAULT_MODEL). */
static void apply_provider_model(tny_ctx *ctx, int id) {
    if (ctx->model_from_flag) return;
    const char *name = tny_provider_name(ctx);
    const char *m = tny_settings_provider_model(ctx, name);
    if (!m && id == TNY_BK_OPENAI && ctx->settings)
        m = jget_str(jget(yyjson_doc_get_root(ctx->settings), name), "model");
    if (!m || !*m) m = derived_env_value(name, "_DEFAULT_MODEL");
    if (m && *m) {
        free(ctx->model);
        ctx->model = xstrdup(m);
    }
}

/* Settings-default reasoning effort (docs/adr/0015), applied once the
 * provider is known. Weakest link in the chain: --effort and /effort
 * (effort_explicit) beat TNY_REASONING_EFFORT (already loaded) beat this.
 * `"effort"` is either one string for every provider or a per-provider
 * object like `"models"`; "default"/empty entries mean provider default. */
static void apply_provider_effort(tny_ctx *ctx) {
    if (ctx->effort_explicit) return;               /* --effort / /effort */
    if (ctx->reasoning_effort && !ctx->effort_from_settings) return; /* env */
    /* (re)compute for the provider that just resolved: a per-provider
     * settings value must not leak into a /provider switch */
    free(ctx->reasoning_effort);
    ctx->reasoning_effort = NULL;
    ctx->effort_from_settings = false;
    if (!ctx->settings) return;
    yyjson_val *e = jget(yyjson_doc_get_root(ctx->settings), "effort");
    const char *v = NULL;
    if (yyjson_is_str(e)) v = yyjson_get_str(e);
    else if (yyjson_is_obj(e)) v = jget_str(e, tny_provider_name(ctx));
    if (v && *v && strcmp(v, "default") != 0) {
        ctx->reasoning_effort = xstrdup(v);
        ctx->effort_from_settings = true;
    }
}

int tny_resolve_backend(tny_ctx *ctx, const char *flag_value) {
    int id = -1;
    const char *custom_name = NULL;
    char *env_pick = NULL;
    if (flag_value) {
        id = tny_backend_from_name(flag_value);
        if (id == -1 && tny_custom_provider_exists(ctx, flag_value)) {
            id = TNY_BK_OPENAI;
            custom_name = flag_value;
        }
        if (id == -1) {
            fprintf(stderr,
                    "tny: unknown provider '%s' (cursor|codex|acp|openai, a "
                    "settings.json object with a base_url, or NAME_BASE_URL "
                    "in the environment)\n", flag_value);
            return -1;
        }
    }
    if (id < 0) { /* the provider (and model) last used wins over detection */
        const char *last = tny_settings_get_str(ctx, "last_provider");
        if (!last) last = tny_settings_get_str(ctx, "last_backend");
        if (last) {
            id = tny_backend_from_name(last);
            if (id == -1 && tny_custom_provider_exists(ctx, last)) {
                id = TNY_BK_OPENAI;
                custom_name = last;
            }
        }
    }
    if (id == -1) {
        const char *e1 = getenv("OPENAI_BASE_URL"), *e2 = getenv("OPENAI_API_KEY");
        if ((e1 && *e1) || (e2 && *e2)) id = TNY_BK_OPENAI;
    }
    if (id == -1 && (env_pick = env_sole_detected_provider()) != NULL) {
        id = TNY_BK_OPENAI; /* exactly one NAME_BASE_URL + NAME_API_KEY pair */
        custom_name = env_pick;
    }
    /* No explicit choice anywhere: prefer subscription logins over raw keys
     * (docs/cli.md "Provider selection"). Codex login first, then a Cursor
     * key from the environment, then the openai backend's own error path. */
    if (id == -1 && tny_codex_auth_present()) id = TNY_BK_CODEX;
    if (id == -1) {
        const char *ck = getenv("CURSOR_API_KEY");
        if (ck && *ck) id = TNY_BK_CURSOR;
    }
    if (id == -1) id = TNY_BK_OPENAI;
    ctx->backend = id;
    if (custom_name) {
        apply_custom_provider(ctx, custom_name);
    } else if (ctx->provider_name) {
        /* switching away from a named profile (TUI /provider): restore the
         * builtin openai config the profile replaced */
        free(ctx->provider_name);
        ctx->provider_name = NULL;
        load_openai_profile(ctx);
    }
    apply_provider_model(ctx, id);
    apply_provider_effort(ctx);
    free(env_pick);
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
    const char *name = tny_provider_name(ctx);
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
    free(ctx->provider_name);
    free(ctx->model);
    free(ctx->base_url);
    free(ctx->api_key);
    free(ctx->auth_header_name);
    free(ctx->auth_header_prefix);
    free(ctx->max_tokens_field);
    free(ctx->output_schema);
    free(ctx->bridge_bin);
    free(ctx->codex_ws);
    free(ctx->codex_bin);
    free(ctx->ws_token_file);
    free(ctx->service_tier);
    free(ctx->reasoning_effort);
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

char *tny_provider_names_joined(tny_ctx *ctx) {
    buf_t b;
    buf_init(&b);
    for (int i = 0; i < TNY_BK_COUNT; i++)
        buf_appendf(&b, "%s%s", i ? "|" : "", tny_backend_name((tny_backend_id)i));
    yyjson_val *root = ctx && ctx->settings ? yyjson_doc_get_root(ctx->settings) : NULL;
    size_t idx, max;
    yyjson_val *k, *v;
    if (yyjson_is_obj(root)) yyjson_obj_foreach(root, idx, max, k, v) {
        const char *name = yyjson_get_str(k);
        if (!name || !yyjson_is_obj(v)) continue;
        const char *bu = jget_str(v, "base_url");
        if (!bu || !*bu || !tny_custom_provider_exists(ctx, name)) continue;
        buf_appendf(&b, "|%s", name);
    }
    int n_env = 0;
    char **env = tny_env_provider_names(&n_env);
    for (int i = 0; i < n_env; i++) {
        const char *in_settings = jget_str(jget(root, env[i]), "base_url");
        if (!in_settings || !*in_settings) buf_appendf(&b, "|%s", env[i]);
        free(env[i]);
    }
    free(env);
    return b.data;
}
