#include "core/config.h"
#include "core/cursor_config.h"
#include "core/tasks.h"
#include "core/backend.h"
#include "core/extensions.h"
#include "core/instructions.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

const char *tny_perm_mode_name(tny_perm_mode m) {
    switch (m) {
    case TNY_MODE_AUTO: return "auto";
    case TNY_MODE_YOLO: return "yolo";
    default: return "ask";
    }
}

/* TNY_MODE_ASK < TNY_MODE_AUTO < TNY_MODE_YOLO in authority. */
static bool parse_perm_mode(const char *name, tny_perm_mode *out) {
    if (!name) return false;
    if (strcmp(name, "ask") == 0) *out = TNY_MODE_ASK;
    else if (strcmp(name, "auto") == 0) *out = TNY_MODE_AUTO;
    else if (strcmp(name, "yolo") == 0) *out = TNY_MODE_YOLO;
    else return false;
    return true;
}

bool tny_nested_perm_mode(tny_perm_mode *parent) {
    const char *nested = getenv("TNY_NESTED");
    if (!nested || strcmp(nested, "1") != 0) return false;
    tny_perm_mode mode = TNY_MODE_ASK;
    if (!parse_perm_mode(getenv("TNY_NESTED_MODE"), &mode)) mode = TNY_MODE_ASK;
    if (parent) *parent = mode;
    return true;
}

bool tny_perm_mode_allowed_nested(tny_perm_mode requested, char *err, size_t errlen) {
    tny_perm_mode parent = TNY_MODE_ASK;
    if (!tny_nested_perm_mode(&parent) || requested <= parent) return true;
    if (err && errlen)
        snprintf(err, errlen,
                 "permission mode '%s' is wider than the '%s' mode of the tny turn this command "
                 "runs inside; a nested run cannot widen it",
                 tny_perm_mode_name(requested), tny_perm_mode_name(parent));
    return false;
}

int tny_parse_max_steps(const char *s) {
    if (!s || !*s) return -1;
    if (strcmp(s, "unlimited") == 0 || strcmp(s, "none") == 0) return 0;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9') return -1;
    long v = strtol(s, NULL, 10);
    if (v < 0 || v > 1000000) return -1;
    return (int)v;
}

void tny_color_resolve(const tny_ctx *ctx, bool tty, bool *color, bool *attr) {
    const char *f = getenv("CLICOLOR_FORCE");
    bool force = ctx->force_color || (f && *f && strcmp(f, "0") != 0);
    if (ctx->no_color) {
        *color = false;
        *attr = false;
        return;
    }
    if (force) {
        *color = true;
        *attr = true;
        return;
    }
    *attr = tty;
    *color = tty && !getenv("NO_COLOR");
}

bool tny_tier_is_fast(const char *tier) {
    return tier && (strcmp(tier, "fast") == 0 || strcmp(tier, "priority") == 0);
}

bool tny_wire_is_chat(const char *wire_api) { return wire_api && strcmp(wire_api, "chat") == 0; }

static const char *bk_names[TNY_BK_COUNT] = {"openai", "cursor", "acp"};

const char *tny_tool_profile_name(tny_tool_profile profile) {
    if (profile == TNY_TOOLS_TERMINAL_EDIT) return "terminal+edit";
    if (profile == TNY_TOOLS_TERMINAL) return "terminal";
    return "all";
}

bool tny_tool_profile_is_shell(const tny_ctx *ctx) {
    return ctx && !ctx->library_mode && ctx->tool_profile != TNY_TOOLS_ALL;
}

void tny_tool_profile_ignore(tny_ctx *ctx, const char *surface) {
    if (!ctx || ctx->tool_profile == TNY_TOOLS_ALL) return;
    fprintf(stderr, "tny: tool profile %s ignored in %s; using all\n",
            tny_tool_profile_name(ctx->tool_profile), surface ? surface : "this mode");
    ctx->tool_profile = TNY_TOOLS_ALL;
}

static bool tool_profile_parse(const char *value, tny_tool_profile *profile) {
    if (!value || !profile) return false;
    if (strcmp(value, "all") == 0) *profile = TNY_TOOLS_ALL;
    else if (strcmp(value, "terminal+edit") == 0) *profile = TNY_TOOLS_TERMINAL_EDIT;
    else if (strcmp(value, "terminal") == 0) *profile = TNY_TOOLS_TERMINAL;
    else return false;
    return true;
}

/* Canonical levels (TNY_EFFORT_LEVELS) and their per-provider wire words.
 * Providers advertise more values than they share ("minimal", "ultra", …);
 * those pass through tny_effort_wire verbatim so the catalog stays usable. */
static const struct {
    const char *level;
    const char *openai; /* reasoning_effort / reasoning.effort (codex too) */
    const char *cursor; /* ModelSelection params candidate value */
} EFFORTS[] = {
    /* "max" is not an OpenAI chat-completions value: clamp it to xhigh. */
    {"off", "none", "none"},  {"light", "low", "low"},     {"medium", "medium", "medium"},
    {"high", "high", "high"}, {"xhigh", "xhigh", "xhigh"}, {"max", "xhigh", "max"},
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
        case TNY_BK_CURSOR: return EFFORTS[i].cursor;
        default: return EFFORTS[i].level;
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
    return ctx->provider_name ? ctx->provider_name : tny_backend_name((tny_backend_id)ctx->backend);
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

/* Named ACP agents live in a namespace separate from OpenAI-compatible
 * profiles. The canonical settings shape is `acp.NAME`; the earlier
 * `acp.agents.NAME` nesting remains readable for compatibility. Selectors use
 * `acp@NAME` (preferred) or the legacy `acp:NAME` spelling. */
static const char *acp_provider_name(const char *provider) {
    if (!provider) return NULL;
    if (str_starts(provider, "acp@") || str_starts(provider, "acp:"))
        return provider[4] ? provider + 4 : NULL;
    return NULL;
}

static yyjson_val *acp_profiles_obj(tny_ctx *ctx) {
    if (!ctx || !ctx->settings) return NULL;
    yyjson_val *acp = jget(yyjson_doc_get_root(ctx->settings), "acp");
    if (!yyjson_is_obj(acp)) return NULL;
    yyjson_val *legacy = jget(acp, "agents");
    return yyjson_is_obj(legacy) ? legacy : acp;
}

static yyjson_val *acp_profile_obj(tny_ctx *ctx, const char *provider) {
    const char *name = acp_provider_name(provider);
    if (!name) return NULL;
    yyjson_val *profile = jget(acp_profiles_obj(ctx), name);
    return yyjson_is_obj(profile) ? profile : NULL;
}

bool tny_acp_profile_exists(tny_ctx *ctx, const char *provider) {
    return acp_profile_obj(ctx, provider) != NULL;
}

static bool acp_profile_name_valid(const char *name) {
    if (!name || !*name) return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            return false;
    }
    return true;
}

static void clear_profile_agent(tny_ctx *ctx) {
    if (!ctx->agent_from_profile) return;
    for (char **p = ctx->agent_argv; p && *p; p++) free(*p);
    free(ctx->agent_argv);
    ctx->agent_argv = NULL;
    ctx->agent_from_profile = false;
}

/* Validate only when selected, then copy every argv string out of yyjson's
 * document storage. Returns -1 with a user-facing diagnostic on bad config. */
static int apply_acp_profile(tny_ctx *ctx, const char *provider) {
    const char *name = acp_provider_name(provider);
    if (!acp_profile_name_valid(name)) {
        fprintf(stderr,
                "tny: invalid ACP provider '%s': names use letters, "
                "digits, - and _\n",
                provider);
        return -1;
    }
    yyjson_val *profile = acp_profile_obj(ctx, provider);
    if (!profile) {
        fprintf(stderr,
                "tny: ACP provider '%s' is not defined under "
                "settings.json acp.%s\n",
                provider, name);
        return -1;
    }
    yyjson_val *command_val = jget(profile, "command");
    yyjson_val *args = jget(profile, "args");
    bool legacy_command = yyjson_is_arr(command_val);
    const char *command = yyjson_is_str(command_val) ? yyjson_get_str(command_val) : NULL;
    if ((!command || !*command) && !legacy_command) {
        fprintf(stderr,
                "tny: settings.json acp.%s.command must be a "
                "nonempty string\n",
                name);
        return -1;
    }
    if (legacy_command && args) {
        fprintf(stderr,
                "tny: settings.json acp.%s cannot combine legacy "
                "command array with args\n",
                name);
        return -1;
    }
    if (args && !yyjson_is_arr(args)) {
        fprintf(stderr,
                "tny: settings.json acp.%s.args must be a string "
                "array when present\n",
                name);
        return -1;
    }
    yyjson_val *parts = legacy_command ? command_val : args;
    size_t nparts = yyjson_is_arr(parts) ? yyjson_arr_size(parts) : 0;
    size_t argc = legacy_command ? nparts : nparts + 1;
    if (argc == 0) {
        fprintf(stderr, "tny: settings.json acp.%s.command must not be empty\n", name);
        return -1;
    }
    char **argv = calloc(argc + 1, sizeof *argv);
    if (!argv) return -1; /* OOM: no observable profile state changed */
    size_t out = 0, idx, max;
    yyjson_val *v;
    if (!legacy_command) argv[out++] = xstrdup(command);
    if ((!legacy_command && !argv[0])) {
        free(argv);
        return -1;
    }
    if (yyjson_is_arr(parts)) yyjson_arr_foreach(parts, idx, max, v) {
            const char *arg = yyjson_is_str(v) ? yyjson_get_str(v) : NULL;
            if (!arg || !*arg) {
                fprintf(stderr,
                        "tny: settings.json acp.%s.%s[%zu] must be a "
                        "nonempty string\n",
                        name, legacy_command ? "command" : "args", idx);
                for (size_t i = 0; i < out; i++) free(argv[i]);
                free(argv);
                return -1;
            }
            argv[out] = xstrdup(arg);
            if (!argv[out]) {
                for (size_t i = 0; i < out; i++) free(argv[i]);
                free(argv);
                return -1; /* OOM: allocator fault injection is out of scope */
            }
            out++;
        }
    if ((str_starts(argv[0], "ws://") || str_starts(argv[0], "wss://")) && argc != 1) {
        fprintf(stderr,
                "tny: settings.json acp.%s.args must be empty for a "
                "remote WebSocket agent\n",
                name);
        for (size_t i = 0; i < argc; i++) free(argv[i]);
        free(argv);
        return -1;
    }
    yyjson_val *model_val = jget(profile, "model");
    const char *model = NULL;
    if (model_val) {
        model = yyjson_is_str(model_val) ? yyjson_get_str(model_val) : NULL;
        if (!model || !*model) {
            fprintf(stderr,
                    "tny: settings.json acp.%s.model must be a "
                    "nonempty string when present\n",
                    name);
            for (size_t i = 0; i < argc; i++) free(argv[i]);
            free(argv);
            return -1;
        }
    }
    if (ctx->agent_argv && !ctx->agent_from_profile) {
        fprintf(stderr,
                "tny: --agent cannot be combined with named ACP "
                "provider '%s'; use --provider acp for an ad-hoc "
                "command\n",
                provider);
        for (size_t i = 0; i < argc; i++) free(argv[i]);
        free(argv);
        return -1;
    }
    clear_profile_agent(ctx);
    ctx->agent_argv = argv;
    ctx->agent_from_profile = true;
    free(ctx->provider_name);
    ctx->provider_name = xstrdup(provider);
    if (!ctx->model_from_flag) {
        free(ctx->model);
        ctx->model = model ? xstrdup(model) : NULL;
    }
    return 0;
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
        if (klen <= suf || memcmp(s + klen - suf, "_BASE_URL", suf) != 0) continue;
        size_t plen = klen - suf;
        char *name = malloc(plen + 1);
        if (!name) continue;
        bool ok = true;
        for (size_t i = 0; i != plen; i++) {
            char c = s[i];
            if (c >= 'A' && c <= 'Z') name[i] = (char)(c - 'A' + 'a');
            else if ((c >= '0' && c <= '9') || c == '_') name[i] = c;
            else {
                ok = false;
                break;
            }
        }
        name[plen] = 0;
        if (!ok || tny_backend_from_name(name) >= 0) {
            free(name);
            continue;
        }
        bool dup = false;
        for (int i = 0; i < n; i++)
            if (strcmp(v[i], name) == 0) {
                dup = true;
                break;
            }
        if (dup) {
            free(name);
            continue;
        }
        char **nv = realloc(v, sizeof(char *) * (size_t)(n + 2));
        if (!nv) {
            free(name);
            break;
        }
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
    if (!key || !*key) key = jget_str(oa, "api_key"); /* docs/adr/0018 */
    secure_free(ctx->api_key);
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
    const char *wa = getenv("OPENAI_WIRE_API");
    if (!wa || !*wa) wa = jget_str(oa, "wire_api");
    free(ctx->wire_api);
    ctx->wire_api = wa && *wa ? xstrdup(wa) : NULL; /* NULL = responses */
    tny_ctx_clear_extra_headers(ctx);               /* builtin-profile headers must not leak */
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
    /* a key stored by `tny provider setup --api-key` (docs/adr/0018) is the
     * fallback: an env var always beats it, so rotation via the shell works */
    if (!key || !*key) key = jget_str(o, "api_key");
    secure_free(ctx->api_key);
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
    const char *wa = derived_env_value(name, "_WIRE_API");
    if (!wa) wa = jget_str(o, "wire_api");
    free(ctx->wire_api);
    ctx->wire_api = wa && *wa ? xstrdup(wa) : NULL; /* NULL = responses */
    tny_ctx_clear_extra_headers(ctx);
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
        if (!getcwd(tmp, sizeof tmp)) {
            free(ctx);
            return NULL;
        }
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

    char cursor_err[256] = {0};
    ctx->cursor_config = tny_cursor_config_load(jget(sroot, "cursor"), jget(rroot, "cursor"),
                                                cursor_err, sizeof cursor_err);
    if (!ctx->cursor_config) {
        if (*cursor_err) fprintf(stderr, "tny: %s\n", cursor_err);
        tny_ctx_free(ctx);
        return NULL;
    }

    /* defaults. yolo is deliberate: host providers run their own loops and
     * never hand tny a real approval gate, so tny runs every agent in yolo
     * unless the user explicitly opts into ask/auto (docs/adr/0001). */
    ctx->backend = -1;
    ctx->perm_mode = TNY_MODE_YOLO;
    ctx->tool_profile = TNY_TOOLS_ALL;
    ctx->max_steps = 0; /* unlimited; .tny.json "steps" or --max-steps cap it */
    ctx->extensions_enabled = true;
    ctx->max_extension_iterations = 0; /* unlimited by default */
    ctx->extension_timeout_ms = 5000;
    ctx->max_tool_result_bytes = 32768;
    ctx->context_enabled = true;
    ctx->task_explicit = false;
    ctx->sandbox_mode = xstrdup("auto");

    /* settings-level defaults. Provider/model/effort/fast are completed in
     * tny_resolve_backend because named providers need their effective name. */
    const char *s;
    tny_perm_mode mode = ctx->perm_mode;
    if ((s = jget_str(wso, "permission_mode")) || (s = jget_str(sroot, "permission_mode")))
        parse_perm_mode(s, &mode);
    /* Settings are the machine's default, not a request: a nested run clamps
     * them silently. An explicit environment override is refused instead, so
     * the caller learns the mode it asked for is unavailable (ADR 0063). */
    tny_perm_mode parent = TNY_MODE_ASK;
    if (tny_nested_perm_mode(&parent) && mode > parent) mode = parent;
    ctx->perm_mode = mode;
    const char *pm_env = getenv("TNY_PERMISSION_MODE");
    if (pm_env && parse_perm_mode(pm_env, &mode)) {
        char nested_error[256];
        if (!tny_perm_mode_allowed_nested(mode, nested_error, sizeof nested_error)) {
            fprintf(stderr, "tny: TNY_PERMISSION_MODE: %s\n", nested_error);
            tny_ctx_free(ctx);
            return NULL;
        }
        ctx->perm_mode = mode;
    }

    const char *tools_setting = jget_str(sroot, "tools");
    if (tools_setting && !tool_profile_parse(tools_setting, &ctx->tool_profile))
        fprintf(stderr, "tny: warning: settings.json tools must be all|terminal+edit|terminal\n");
    const char *tools_env = getenv("TNY_TOOLS");
    if (tools_env && !tool_profile_parse(tools_env, &ctx->tool_profile))
        fprintf(stderr, "tny: warning: TNY_TOOLS must be all|terminal+edit|terminal\n");
#ifdef __EMSCRIPTEN__
    tny_tool_profile_ignore(ctx, "wasm");
#endif

    /* Extensions are global user code, never repo authority. Configuration
     * therefore comes only from settings/env/CLI, not .tny.json. */
    yyjson_val *ext = jget(sroot, "extensions");
    if (ext && yyjson_is_obj(ext)) {
        ctx->extensions_enabled = jget_bool(ext, "enabled", true);
        int64_t cap = jget_int(ext, "max_iterations", 0);
        int64_t timeout = jget_int(ext, "timeout_ms", 5000);
        if (cap >= 0 && cap <= 1000000) ctx->max_extension_iterations = (int)cap;
        if (timeout >= 1 && timeout <= 600000) ctx->extension_timeout_ms = (int)timeout;
    }
    const char *ext_env = getenv("TNY_EXTENSIONS");
    if (ext_env &&
        (strcmp(ext_env, "0") == 0 || strcmp(ext_env, "false") == 0 || strcmp(ext_env, "off") == 0))
        ctx->extensions_enabled = false;

    /* Foreign MCP profiles can name executables and carry credentials. Only
     * this global user setting can authorize reading them; repo config never
     * can. An absent/invalid list leaves the mask at zero, so no foreign path
     * is even inspected by the MCP config layer (docs/adr/0051). */
    yyjson_val *mcp = jget(sroot, "mcp");
    yyjson_val *imports = jget(mcp, "import_from");
    if (imports && !yyjson_is_arr(imports)) {
        fprintf(stderr, "tny: warning: settings.json mcp.import_from must be an array\n");
    } else if (imports) {
        size_t idx, max;
        yyjson_val *v;
        yyjson_arr_foreach(imports, idx, max, v) {
            if (!yyjson_is_str(v)) {
                fprintf(stderr,
                        "tny: warning: settings.json mcp.import_from[%zu] must be a string\n", idx);
                continue;
            }
            const char *name = yyjson_get_str(v);
            unsigned bit = 0;
            if (strcmp(name, "codex") == 0) bit = TNY_MCP_IMPORT_CODEX;
            else if (strcmp(name, "claude") == 0) bit = TNY_MCP_IMPORT_CLAUDE;
            else if (strcmp(name, "grok") == 0) bit = TNY_MCP_IMPORT_GROK;
            else if (strcmp(name, "cursor") == 0 || strcmp(name, "cursor-agent") == 0)
                bit = TNY_MCP_IMPORT_CURSOR;
            else {
                fprintf(stderr,
                        "tny: warning: settings.json mcp.import_from: unknown source '%s'\n", name);
                continue;
            }
            if (!(ctx->mcp_import_mask & bit) && ctx->n_mcp_import_sources < 4)
                ctx->mcp_import_order[ctx->n_mcp_import_sources++] = bit;
            ctx->mcp_import_mask |= bit;
        }
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

    /* repo limits — never authority */
    if (rroot) {
        ctx->max_steps = (int)jget_int(rroot, "steps", ctx->max_steps);
        ctx->max_tool_result_bytes =
            (size_t)jget_int(rroot, "max_tool_result_bytes", (int64_t)ctx->max_tool_result_bytes);
        ctx->context_enabled = jget_bool(rroot, "context", ctx->context_enabled);
        const char *sb = jget_str(rroot, "sandbox");
        if (sb) {
            free(ctx->sandbox_mode);
            ctx->sandbox_mode = xstrdup(sb);
        }
    }

    /* saved extra dirs for this workspace */
    yyjson_val *dirs = jget(wso, "additional_dirs");
    if (dirs && yyjson_is_arr(dirs)) {
        size_t idx, max;
        yyjson_val *v;
        yyjson_arr_foreach(dirs, idx, max, v) {
            if (!yyjson_is_str(v)) continue;
            char **grown =
                realloc(ctx->extra_dirs, sizeof(char *) * (size_t)(ctx->n_extra_dirs + 1));
            if (!grown) break;
            ctx->extra_dirs = grown;
            ctx->extra_dirs[ctx->n_extra_dirs++] = xstrdup(yyjson_get_str(v));
        }
    }
    (void)instructions_refresh(ctx);
    if (ctx->extensions_enabled) {
        tny_extensions *extensions =
            tny_extensions_new(ctx->tny_dir, ctx->cwd, ctx->extension_timeout_ms);
        if (extensions && tny_extensions_get_state(extensions) != TNY_EXTENSIONS_EMPTY)
            ctx->extensions = extensions;
        else tny_extensions_free(extensions);
    }
    return ctx;
}

tny_ctx *tny_ctx_new_explicit(const char *cwd, const char *state_dir) {
    if (!cwd || !state_dir || !dir_exists(cwd)) return NULL;
    tny_ctx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return NULL;
    ctx->cwd = path_abs(cwd);
    ctx->tny_dir = path_abs(state_dir);
    if (!ctx->cwd || !ctx->tny_dir) {
        tny_ctx_free(ctx);
        return NULL;
    }
    snprintf(ctx->ws_hash, sizeof ctx->ws_hash, "%016llx",
             (unsigned long long)fnv1a(ctx->cwd, strlen(ctx->cwd)));
    ctx->settings_path = path_join(ctx->tny_dir, "settings.json");

    ctx->backend = TNY_BK_OPENAI;
    ctx->provider_name = xstrdup("openai");
    ctx->perm_mode = TNY_MODE_ASK;
    ctx->tool_profile = TNY_TOOLS_ALL;
    ctx->max_steps = 0;              /* unlimited unless the embedder sets a cap */
    ctx->extensions_enabled = false; /* explicit embedders opt into authority */
    ctx->max_extension_iterations = 0;
    ctx->extension_timeout_ms = 5000;
    ctx->max_tool_result_bytes = 32768;
    ctx->context_enabled = true;
    ctx->mcp_disabled = true;
    ctx->library_mode = true;
    ctx->sandbox_mode = xstrdup("auto");

    ctx->base_url = xstrdup("https://api.openai.com/v1");
    ctx->auth_header_name = xstrdup("Authorization");
    ctx->auth_header_prefix = xstrdup("Bearer ");
    ctx->bridge_bin = xstrdup("cursor-sdk-bridge");
    ctx->cursor_config = tny_cursor_config_load(NULL, NULL, NULL, 0);
    if (!ctx->cursor_config) {
        tny_ctx_free(ctx);
        return NULL;
    }
    (void)instructions_refresh(ctx);
    return ctx;
}

const char *tny_settings_provider_model(tny_ctx *ctx, const char *provider) {
    if (!ctx->settings) return NULL;
    return jget_str(jget(yyjson_doc_get_root(ctx->settings), "models"), provider);
}

/* A scalar applies globally. An object applies per provider. Empty/default
 * mean "use the provider default" for fields that support an unset value. */
static const char *provider_setting(tny_ctx *ctx, const char *key) {
    if (!ctx->settings) return NULL;
    yyjson_val *v = jget(yyjson_doc_get_root(ctx->settings), key);
    if (yyjson_is_str(v)) return yyjson_get_str(v);
    if (yyjson_is_obj(v)) return jget_str(v, tny_provider_name(ctx));
    return NULL;
}

/* Once the provider is known, pick its model: --model beats the explicit
 * scalar/object `model` default, then the remembered `models.NAME` entry,
 * provider profile model, environment fallback, and provider default. */
static void apply_provider_model(tny_ctx *ctx, int id) {
    if (ctx->model_from_flag) return;
    const char *name = tny_provider_name(ctx);
    const char *m = provider_setting(ctx, "model");
    if (!m) m = tny_settings_provider_model(ctx, name);
    if (!m && id == TNY_BK_OPENAI && ctx->settings)
        m = jget_str(jget(yyjson_doc_get_root(ctx->settings), name), "model");
    if (!m && id == TNY_BK_ACP && ctx->provider_name)
        m = jget_str(acp_profile_obj(ctx, ctx->provider_name), "model");
    if ((!m || !*m || strcmp(m, "default") == 0) && !ctx->model)
        m = derived_env_value(name, "_DEFAULT_MODEL");
    if (m && *m && strcmp(m, "default") != 0) {
        free(ctx->model);
        ctx->model = xstrdup(m);
    } else if (m && strcmp(m, "default") == 0) {
        free(ctx->model);
        ctx->model = NULL;
    }
}

/* settings `fast`: boolean true selects the paid tier, false selects the
 * standard tier; string fast|priority|default is also accepted. It is applied
 * after provider resolution so unsupported providers can fail clearly. */
static int apply_provider_fast(tny_ctx *ctx) {
    if (ctx->service_tier_explicit) return 0;
    if (ctx->service_tier_from_settings) {
        free(ctx->service_tier);
        ctx->service_tier = NULL;
        ctx->service_tier_from_settings = false;
    }
    if (!ctx->settings) return 0;
    yyjson_val *v = jget(yyjson_doc_get_root(ctx->settings), "fast");
    if (yyjson_is_obj(v)) v = jget(v, tny_provider_name(ctx));
    const char *tier = NULL;
    if (yyjson_is_bool(v)) tier = yyjson_get_bool(v) ? "fast" : "default";
    else if (yyjson_is_str(v)) tier = yyjson_get_str(v);
    if (!tier || !*tier) return 0;
    if (strcmp(tier, "fast") != 0 && strcmp(tier, "priority") != 0 &&
        strcmp(tier, "default") != 0 && strcmp(tier, "off") != 0) {
        fprintf(stderr, "tny: settings.json fast must be true, false, fast, "
                        "priority, or default\n");
        return -1;
    }
    if (!(tny_backend_caps((tny_backend_id)ctx->backend) & TNY_CAP_FAST)) {
        fprintf(stderr,
                "tny: settings.json fast is not supported by provider "
                "'%s'\n",
                tny_provider_name(ctx));
        return -1;
    }
    free(ctx->service_tier);
    ctx->service_tier = xstrdup(tny_tier_is_fast(tier) ? "fast" : "default");
    ctx->service_tier_from_settings = true;
    return 0;
}

/* Settings-default reasoning effort (docs/adr/0015), applied once the
 * provider is known. Weakest link in the chain: --effort and /effort
 * (effort_explicit) beat TNY_REASONING_EFFORT (already loaded) beat this.
 * `"effort"` is either one string for every provider or a per-provider
 * object like `"models"`; "default"/empty entries mean provider default. */
static void apply_provider_effort(tny_ctx *ctx) {
    if (ctx->effort_explicit) return;                                /* --effort / /effort */
    if (ctx->reasoning_effort && !ctx->effort_from_settings) return; /* env */
    /* (re)compute for the provider that just resolved: a per-provider
     * settings value must not leak into a /provider switch */
    free(ctx->reasoning_effort);
    ctx->reasoning_effort = NULL;
    ctx->effort_from_settings = false;
    if (!ctx->settings) return;
    const char *v = provider_setting(ctx, "effort");
    if (v && *v && strcmp(v, "default") != 0) {
        ctx->reasoning_effort = xstrdup(v);
        ctx->effort_from_settings = true;
    }
}

int tny_resolve_backend(tny_ctx *ctx, const char *flag_value) {
    int id = -1;
    if (!flag_value) flag_value = tny_settings_get_str(ctx, "provider");
    const char *custom_name = NULL;
    const char *builtin_profile = NULL; /* codex|claude|grok (profiles.c) */
    const char *acp_profile = NULL;     /* full selector: acp@NAME */
    char *env_pick = NULL;
    if (flag_value) {
        if (str_starts(flag_value, "acp@") || str_starts(flag_value, "acp:")) {
            id = TNY_BK_ACP; /* apply_acp_profile gives the precise error */
            acp_profile = flag_value;
        } else {
            id = tny_backend_from_name(flag_value);
        }
        /* a user settings profile / env pair shadows a builtin of the
         * same name — explicit config wins over what tny ships */
        if (id == -1 && tny_custom_provider_exists(ctx, flag_value)) {
            id = TNY_BK_OPENAI;
            custom_name = flag_value;
        }
        if (id == -1 && tny_builtin_profile_exists(flag_value)) {
            id = TNY_BK_OPENAI;
            builtin_profile = flag_value;
        }
        if (id == -1) {
            fprintf(stderr,
                    "tny: unknown provider '%s' (cursor|acp|openai|codex|"
                    "claude|grok|acp@NAME, a settings.json object with a "
                    "base_url, or NAME_BASE_URL in the environment)\n",
                    flag_value);
            return -1;
        }
    }
    if (id < 0) { /* the provider (and model) last used wins over detection */
        const char *last = tny_settings_get_str(ctx, "last_provider");
        if (!last) last = tny_settings_get_str(ctx, "last_backend");
        if (last) {
            id = tny_backend_from_name(last);
            if (id == -1 && tny_acp_profile_exists(ctx, last)) {
                id = TNY_BK_ACP;
                acp_profile = last;
            }
            if (id == -1 && tny_custom_provider_exists(ctx, last)) {
                id = TNY_BK_OPENAI;
                custom_name = last;
            }
            if (id == -1 && tny_builtin_profile_exists(last)) {
                id = TNY_BK_OPENAI;
                builtin_profile = last;
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
     * (docs/cli.md "Provider selection"). Codex login first, then a Claude
     * Code OAuth login, then a grok session, then a Cursor key from the
     * environment, then the openai backend's own error path. */
    if (id == -1 && (ctx->chatgpt_token || tny_codex_auth_present())) {
        id = TNY_BK_OPENAI;
        builtin_profile = "codex";
    }
    if (id == -1 && tny_claude_auth_present()) {
        id = TNY_BK_OPENAI;
        builtin_profile = "claude";
    }
    if (id == -1 && tny_grok_auth_present()) {
        id = TNY_BK_OPENAI;
        builtin_profile = "grok";
    }
    if (id == -1) {
        const char *ck = getenv("CURSOR_API_KEY");
        if (ck && *ck) id = TNY_BK_CURSOR;
    }
    if (id == -1) id = TNY_BK_OPENAI;
    if (acp_profile) {
        if (apply_acp_profile(ctx, acp_profile) != 0) {
            free(env_pick);
            return -1;
        }
    } else {
        /* Profile-owned argv is meaningful only for its namespaced ACP
         * provider. Ad-hoc --provider acp --agent argv is not profile-owned
         * and remains untouched. */
        clear_profile_agent(ctx);
        if (!ctx->model_from_flag) {
            free(ctx->model);
            ctx->model = NULL;
        }
    }
    ctx->backend = id;
    if (builtin_profile && tny_custom_provider_exists(ctx, builtin_profile)) {
        custom_name = builtin_profile; /* user config shadows the builtin */
        builtin_profile = NULL;
    }
    if (acp_profile) {
        /* apply_acp_profile already installed provider_name + argv/model. */
    } else if (custom_name) {
        apply_custom_provider(ctx, custom_name);
    } else if (builtin_profile) {
        tny_apply_builtin_profile(ctx, builtin_profile);
    } else if (ctx->provider_name) {
        /* switching away from a named profile (TUI /provider): restore the
         * builtin openai config the profile replaced */
        free(ctx->provider_name);
        ctx->provider_name = NULL;
        load_openai_profile(ctx);
    }
    apply_provider_model(ctx, id);
    apply_provider_effort(ctx);
    if (apply_provider_fast(ctx) != 0) {
        free(env_pick);
        return -1;
    }
    tny_finish_builtin_profile(ctx);
    tny_extensions_set_provider(ctx->extensions, (tny_backend_id)ctx->backend);
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

struct kv {
    const char *k, *v;
};

static void edit_set_str(yyjson_mut_doc *doc, yyjson_mut_val *root, void *ud) {
    struct kv *kv = ud;
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, kv->k), yyjson_mut_strcpy(doc, kv->v));
}

int tny_settings_set_str(tny_ctx *ctx, const char *key, const char *value) {
    struct kv kv = {key, value};
    return settings_edit(ctx, edit_set_str, &kv);
}

/* ---- `provider setup` writer (docs/adr/0018) ---- */

struct profile_edit {
    const char *name; /* "openai" or a custom profile name */
    const tny_provider_fields *f;
};

static void edit_provider_profile(yyjson_mut_doc *doc, yyjson_mut_val *root, void *ud) {
    struct profile_edit *pe = ud;
    yyjson_mut_val *o = yyjson_mut_obj_get(root, pe->name);
    if (!o || !yyjson_mut_is_obj(o)) {
        o = yyjson_mut_obj(doc);
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, pe->name), o);
    }
    const struct {
        const char *k, *v;
    } kv[] = {
        {"base_url", pe->f->base_url},       {"api_key", pe->f->api_key},
        {"api_key_env", pe->f->api_key_env}, {"model", pe->f->model},
        {"wire_api", pe->f->wire_api},
    };
    for (size_t i = 0; i < sizeof kv / sizeof *kv; i++)
        if (kv[i].v)
            yyjson_mut_obj_put(o, yyjson_mut_strcpy(doc, kv[i].k), yyjson_mut_strcpy(doc, kv[i].v));
    /* a stored key and a key env var are alternatives: setting one clears
     * the other so the effective source is what the user just chose */
    if (pe->f->api_key) yyjson_mut_obj_remove_key(o, "api_key_env");
    if (pe->f->api_key_env) yyjson_mut_obj_remove_key(o, "api_key");
}

/* Merge fields into the profile `name` ("openai" or a custom name; other
 * builtins are rejected). NULL fields keep their current values. When a
 * raw api_key is stored, settings.json drops to 0600. 0 ok, -1 error
 * (reason in errbuf). */
int tny_provider_write_profile(tny_ctx *ctx, const char *name, const tny_provider_fields *f,
                               char *errbuf, size_t errlen) {
    if (!name || !*name) {
        snprintf(errbuf, errlen, "provider setup needs a name");
        return -1;
    }
    int builtin = tny_backend_from_name(name);
    if (builtin >= 0 && builtin != TNY_BK_OPENAI) {
        snprintf(errbuf, errlen,
                 "'%s' is a host provider; only openai-compatible profiles "
                 "take base_url/api_key",
                 name);
        return -1;
    }
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            snprintf(errbuf, errlen, "provider names use letters, digits, - and _ (got '%s')",
                     name);
            return -1;
        }
    }
    /* reserved top-level settings keys must never become profiles */
    static const char *const reserved[] = {"$schema",
                                           "workspaces",
                                           "models",
                                           "model",
                                           "provider",
                                           "fast",
                                           "permission",
                                           "permission_mode",
                                           "effort",
                                           "extensions",
                                           "acp",
                                           "last_provider",
                                           "last_backend",
                                           "web_search_url",
                                           "web_search_command",
                                           "mcp",
                                           NULL};
    for (int i = 0; reserved[i]; i++)
        if (strcmp(name, reserved[i]) == 0) {
            snprintf(errbuf, errlen, "'%s' is a reserved settings key", name);
            return -1;
        }
    bool exists = builtin == TNY_BK_OPENAI || custom_provider_obj(ctx, name) != NULL;
    if (!exists && (!f->base_url || !*f->base_url)) {
        snprintf(errbuf, errlen,
                 "new provider '%s' needs --base-url (an OpenAI-compatible "
                 "/v1 endpoint)",
                 name);
        return -1;
    }
    struct profile_edit pe = {name, f};
    if (settings_edit(ctx, edit_provider_profile, &pe) != 0) {
        snprintf(errbuf, errlen, "could not write %s", ctx->settings_path);
        return -1;
    }
    if (f->api_key && *f->api_key) chmod(ctx->settings_path, 0600); /* it now holds a secret */
    return 0;
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
    yyjson_mut_obj_put(models, yyjson_mut_strcpy(doc, kv->k), yyjson_mut_strcpy(doc, kv->v));
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

struct ws_edit {
    tny_ctx *ctx;
    const char *dir;
    int op;
}; /* 0 add, 1 remove, 2 clear */

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
            if (!v) break;
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
    if (rc == 0) {
        for (int i = 0; i < ctx->n_extra_dirs; i++) {
            if (strcmp(ctx->extra_dirs[i], abs) == 0) {
                free(ctx->extra_dirs[i]);
                memmove(ctx->extra_dirs + i, ctx->extra_dirs + i + 1,
                        sizeof(char *) * (size_t)(ctx->n_extra_dirs - i - 1));
                ctx->n_extra_dirs--;
                break;
            }
        }
        char **next = realloc(ctx->extra_dirs, sizeof(char *) * (size_t)(ctx->n_extra_dirs + 1));
        if (next) {
            ctx->extra_dirs = next;
            ctx->extra_dirs[ctx->n_extra_dirs++] = xstrdup(abs);
        }
    }
    free(abs);
    return rc;
}

int tny_workspace_remove(tny_ctx *ctx, const char *dir) {
    char *abs = path_abs(dir);
    struct ws_edit e = {ctx, abs ? abs : dir, 1};
    int rc = settings_edit(ctx, edit_ws, &e);
    if (rc == 0) {
        const char *target = abs ? abs : dir;
        for (int i = 0; i < ctx->n_extra_dirs; i++) {
            if (strcmp(ctx->extra_dirs[i], target) != 0) continue;
            free(ctx->extra_dirs[i]);
            memmove(ctx->extra_dirs + i, ctx->extra_dirs + i + 1,
                    sizeof(char *) * (size_t)(ctx->n_extra_dirs - i - 1));
            ctx->n_extra_dirs--;
            break;
        }
    }
    free(abs);
    return rc;
}

int tny_workspace_clear(tny_ctx *ctx) {
    struct ws_edit e = {ctx, NULL, 2};
    int rc = settings_edit(ctx, edit_ws, &e);
    if (rc == 0) {
        for (int i = 0; i < ctx->n_extra_dirs; i++) free(ctx->extra_dirs[i]);
        free(ctx->extra_dirs);
        ctx->extra_dirs = NULL;
        ctx->n_extra_dirs = 0;
    }
    return rc;
}

void tny_ctx_free(tny_ctx *ctx) {
    if (!ctx) return;
    free(ctx->cwd);
    for (int i = 0; i < ctx->n_extra_dirs; i++) free(ctx->extra_dirs[i]);
    free(ctx->extra_dirs);
    free(ctx->provider_name);
    free(ctx->model);
    free(ctx->base_url);
    secure_free(ctx->api_key);
    free(ctx->auth_header_name);
    free(ctx->auth_header_prefix);
    free(ctx->max_tokens_field);
    free(ctx->wire_api);
    free(ctx->output_schema);
    tny_ctx_clear_extra_headers(ctx);
    free(ctx->bridge_bin);
    tny_cursor_config_free(ctx->cursor_config);
    if (ctx->chatgpt_token) secure_free(ctx->chatgpt_token);
    free(ctx->chatgpt_account_id);
    free(ctx->ssh_host);
    free(ctx->ssh_cwd);
    free(ctx->ssh_control);
    free(ctx->service_tier);
    free(ctx->system_prompt);
    free(ctx->task_name);
    free(ctx->task_source);
    free(ctx->task_instructions);
    free(ctx->reasoning_effort);
    free(ctx->instructions_snapshot);
    for (int i = 0; i < ctx->n_instruction_paths; i++) free(ctx->instruction_paths[i]);
    free(ctx->instruction_paths);
    if (ctx->agent_argv) {
        for (char **p = ctx->agent_argv; *p; p++) free(*p);
        free(ctx->agent_argv);
    }
    free(ctx->sandbox_mode);
    free(ctx->tny_dir);
    free(ctx->settings_path);
    tny_extensions_free(ctx->extensions);
    yyjson_doc_free(ctx->settings);
    yyjson_doc_free(ctx->repo_cfg);
    free(ctx);
}

char *tny_provider_names_joined(tny_ctx *ctx) {
    buf_t b;
    buf_init(&b);
    for (int i = 0; i < TNY_BK_COUNT; i++)
        buf_appendf(&b, "%s%s", i ? "|" : "", tny_backend_name((tny_backend_id)i));
    buf_appends(&b, "|codex|claude|grok"); /* builtin profiles (docs/adr/0019) */
    yyjson_val *root = ctx && ctx->settings ? yyjson_doc_get_root(ctx->settings) : NULL;
    size_t idx, max;
    yyjson_val *k, *v;
    if (yyjson_is_obj(root)) yyjson_obj_foreach(root, idx, max, k, v) {
            const char *name = yyjson_get_str(k);
            if (!name || !yyjson_is_obj(v)) continue;
            if (tny_builtin_profile_exists(name)) continue; /* already listed */
            const char *bu = jget_str(v, "base_url");
            if (!bu || !*bu || !tny_custom_provider_exists(ctx, name)) continue;
            buf_appendf(&b, "|%s", name);
        }
    yyjson_val *agents = acp_profiles_obj(ctx);
    if (yyjson_is_obj(agents)) yyjson_obj_foreach(agents, idx, max, k, v) {
            const char *name = yyjson_get_str(k);
            /* Invalid commands are still listed: selection validates them and
             * reports the exact field. Invalid names cannot form a provider ID. */
            if (!name || strcmp(name, "agents") == 0 || !acp_profile_name_valid(name) ||
                !yyjson_is_obj(v))
                continue;
            buf_appendf(&b, "|acp@%s", name);
        }
    int n_env = 0;
    char **env = tny_env_provider_names(&n_env);
    for (int i = 0; i < n_env; i++) {
        const char *in_settings = jget_str(jget(root, env[i]), "base_url");
        if ((!in_settings || !*in_settings) && !tny_builtin_profile_exists(env[i]))
            buf_appendf(&b, "|%s", env[i]);
        free(env[i]);
    }
    free(env);
    return b.data;
}
