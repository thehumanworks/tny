/* cursor.c — the Cursor SDK Bridge backend (docs/backends/cursor-bridge.md).
 *
 * tny spawns `cursor-sdk-bridge`, reads the ready line off its stderr and then
 * speaks Connect (JSON codec) over HTTP/1.1 to the loopback port it printed.
 * The bridge owns the agent loop: tools, sandbox and hooks run inside Cursor,
 * so this backend is a transport plus an event translator, never a tool loop.
 *
 * Request/response field names are the release protos rendered as Connect JSON
 * (camelCase). Where the shape is not pinned in the docs it is built by exactly
 * one function here so a proto bump is a one-line change.
 */
#include "backends/cursor/impl.h"
#include "backends/cursor/options.h"
#include "core/cursor_config.h"
#include "core/image.h"
#include "lib/custom_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#define READY_TIMEOUT_MS                 30000
#define RPC_TIMEOUT_MS                   30000
#define SHUTDOWN_GRACE_S                 5
#define OBSERVE_RECOVERY_MAX_NO_PROGRESS 4u
#define OBSERVE_RECOVERY_BASE_DELAY_MS   50

char *cu_ephemeral_root_create(char *err, size_t errlen) {
    char template[] = "/tmp/tny-cursor-ephemeral.XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        snprintf(err, errlen, "cursor: could not create an ephemeral bridge state directory");
        return NULL;
    }
    char *copy = xstrdup(root);
    if (!copy) {
        (void)rmdir(root);
        snprintf(err, errlen, "cursor: out of memory creating ephemeral bridge state");
    }
    return copy;
}

static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char *child = path_join(path, entry->d_name);
            struct stat st;
            if (child && lstat(child, &st) == 0) {
                if (S_ISDIR(st.st_mode)) remove_tree(child);
                else (void)unlink(child);
            }
            free(child);
        }
        closedir(dir);
    }
    (void)rmdir(path);
}

void cu_ephemeral_root_remove(char **root) {
    if (!root || !*root) return;
    remove_tree(*root);
    free(*root);
    *root = NULL;
}

int cu_append_images(buf_t *body, const char **images, char *err, size_t errlen) {
    if (!images || !images[0]) return 0;
    int count = 0;
    while (images[count]) {
        if (++count > 16) {
            snprintf(err, errlen, "cursor: too many images (max 16)");
            return -1;
        }
    }
    buf_appends(body, ",\"images\":[");
    for (int i = 0; i < count; i++) {
        struct stat st;
        if (stat(images[i], &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
            snprintf(err, errlen, "cannot read image %s", images[i]);
            return -1;
        }
        size_t bytes = (size_t)st.st_size;
        size_t encoded = 4u * ((bytes + 2u) / 3u);
        size_t overhead = 96u + strlen(images[i]);
        if (encoded > CURSOR_MAX_MSG_BYTES || body->len > CURSOR_MAX_MSG_BYTES - encoded ||
            body->len + encoded > CURSOR_MAX_MSG_BYTES - overhead) {
            snprintf(err, errlen, "cursor: encoded image request exceeds %u bytes",
                     CURSOR_MAX_MSG_BYTES);
            return -1;
        }
        size_t len = 0;
        const char *mime = NULL;
        uint8_t *data = image_load(images[i], &len, &mime, err, errlen);
        if (!data) return -1;
        if (i) buf_appends(body, ",");
        buf_appends(body, "{\"data\":{\"data\":\"");
        b64_encode(data, len, body);
        free(data);
        buf_appends(body, "\",\"mimeType\":");
        jescape(body, mime);
        buf_appends(body, "}}");
    }
    buf_appends(body, "]");
    return buf_oom(body) ? -1 : 0;
}

/* ---------- events ---------- */

void cu_emit(cu_impl *o, const tny_backend_event *ev) {
    if (o->cb) o->cb(ev, o->ud);
}

void cu_emit_text(cu_impl *o, tny_event_kind k, const char *t, size_t n) {
    tny_backend_event ev = {0};
    ev.kind = k;
    ev.text = t;
    ev.text_len = n;
    cu_emit(o, &ev);
}

void cu_end_turn(cu_impl *o, tny_stop_reason stop) {
    if (o->ended) return;
    o->ended = true;
    o->active = false;
    if (!o->usage_sent && (o->in_tok || o->out_tok || o->has_cost)) {
        o->usage_sent = true;
        tny_backend_event u = {0};
        u.kind = TNY_EV_USAGE;
        u.in_tokens = o->in_tok;
        u.out_tokens = o->out_tok;
        u.cost = o->cost;
        u.has_cost = o->has_cost;
        cu_emit(o, &u);
    }
    tny_backend_event ev = {0};
    ev.kind = TNY_EV_TURN_END;
    ev.stop = stop;
    cu_emit(o, &ev);
}

/* ---------- request bodies ---------- */

/* TNY_CAP_FAST: the fast tier is a per-model parameter, not a request
 * field. Omitting it keeps the model's own default variant; "default"
 * pins the standard one explicitly. Emits one params entry (with a leading
 * comma when `first` is false) or nothing. Returns true when it wrote. */
bool cursor_append_fast_param(buf_t *b, const char *tier, bool first) {
    if (!tier || !*tier) return false;
    buf_appendf(b, "%s{\"id\":\"fast\",\"value\":\"%s\"}", first ? "" : ",",
                tny_tier_is_fast(tier) ? "true" : "false");
    return true;
}

/* Backward-compatible wrapper: the full ",\"params\":[…]" tail for a tier. */
void cursor_append_model_params(buf_t *b, const char *tier) {
    if (!tier || !*tier) return;
    buf_appends(b, ",\"params\":[");
    cursor_append_fast_param(b, tier, true);
    buf_appends(b, "]");
}

/* AgentOptions (proto/sdk/v1/sdk_messages.proto): model is a ModelSelection
 * ({"id":…}), local.cwd carries at most one entry, extra roots go in
 * local.dirs. Local agents need an explicit model and a cwd. */
static char *rpc(cu_impl *o, cursor_sdk_rpc_id id, const char *body, char *err, size_t errlen) {
    cursor_sdk_error sdk_error;
    cursor_sdk_error_init(&sdk_error);
    char *result =
        cursor_sdk_invoke_unary(&o->sdk, id, body, RPC_TIMEOUT_MS, &sdk_error, err, errlen);
    cursor_sdk_error_free(&sdk_error);
    return result;
}

/* Pull an agent id out of a CreateAgent / ResumeAgent response. */
static char *parse_agent_id(const char *json) {
    yyjson_doc *doc = jparse(json, strlen(json));
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *id = jget_str(root, "agentId");
    if (!id) id = jget_str(root, "agent_id");
    if (!id) {
        yyjson_val *a = jget(root, "agent");
        id = jget_str(a, "id");
        if (!id) id = jget_str(a, "agentId");
    }
    if (!id) id = jget_str(root, "id");
    char *out = id && *id ? xstrdup(id) : NULL;
    yyjson_doc_free(doc);
    return out;
}

/* options.c preserves the complete configured object. Overlay only tny's
 * resolved ModelSelection values, retaining future fields and unrelated
 * model parameters verbatim. Takes ownership of json. */
static char *overlay_model_selection(cu_impl *o, char *json, bool include_model, char *err,
                                     size_t errlen) {
    if (!json || !o->model) return json;
    yyjson_doc *parsed = jparse(json, strlen(json));
    free(json);
    if (!parsed) {
        snprintf(err, errlen, "cursor: generated options are not valid JSON");
        return NULL;
    }
    yyjson_mut_doc *doc = yyjson_doc_mut_copy(parsed, jallocator());
    yyjson_doc_free(parsed);
    if (!doc) {
        snprintf(err, errlen, "cursor: could not allocate model options");
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!include_model) {
        yyjson_mut_obj_remove_key(root, "model");
        char *out = jwrite(doc);
        yyjson_mut_doc_free(doc);
        if (!out) snprintf(err, errlen, "cursor: could not serialize send options");
        return out;
    }
    yyjson_mut_val *model = yyjson_mut_obj_get(root, "model");
    if (!yyjson_mut_is_obj(model)) {
        model = yyjson_mut_obj(doc);
        yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, "model"), model);
    }
    yyjson_mut_obj_put(model, yyjson_mut_strcpy(doc, "id"), yyjson_mut_strcpy(doc, o->model));

    bool effort = o->effort_param && o->effort_value;
    const char *tier = o->ctx ? o->ctx->service_tier : NULL;
    if (effort || (tier && *tier)) {
        yyjson_mut_val *params = yyjson_mut_arr(doc);
        yyjson_mut_val *old = yyjson_mut_obj_get(model, "params");
        size_t idx, max;
        yyjson_mut_val *existing_param;
        if (yyjson_mut_is_arr(old)) {
            yyjson_mut_arr_foreach(old, idx, max, existing_param) {
                const char *id = yyjson_mut_get_str(yyjson_mut_obj_get(existing_param, "id"));
                if (id && ((effort && strcmp(id, o->effort_param) == 0) || strcmp(id, "fast") == 0))
                    continue;
                yyjson_mut_val *copy = yyjson_mut_val_mut_copy(doc, existing_param);
                if (copy) yyjson_mut_arr_add_val(params, copy);
            }
        }
        if (effort) {
            yyjson_mut_val *effort_param = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, effort_param, "id", o->effort_param);
            yyjson_mut_obj_add_strcpy(doc, effort_param, "value", o->effort_value);
            yyjson_mut_arr_add_val(params, effort_param);
        }
        if (tier && *tier) {
            yyjson_mut_val *fast_param = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, fast_param, "id", "fast");
            yyjson_mut_obj_add_strcpy(doc, fast_param, "value",
                                      tny_tier_is_fast(tier) ? "true" : "false");
            yyjson_mut_arr_add_val(params, fast_param);
        }
        yyjson_mut_obj_put(model, yyjson_mut_strcpy(doc, "params"), params);
    }
    char *out = jwrite(doc);
    yyjson_mut_doc_free(doc);
    if (!out) snprintf(err, errlen, "cursor: could not serialize model options");
    return out;
}

static bool configured_send_model(const cu_impl *o) {
    const char *json =
        o->ctx && o->ctx->cursor_config ? o->ctx->cursor_config->send_options_json : NULL;
    if (!json) return false;
    yyjson_doc *doc = jparse(json, strlen(json));
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool present = jget(root, "model") != NULL;
    yyjson_doc_free(doc);
    return present;
}

#ifndef __EMSCRIPTEN__
static bool json_store_is_custom(const char *json) {
    if (!json) return false;
    yyjson_doc *doc = jparse(json, strlen(json));
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool custom = strcmp(jget_str(root, "type") ? jget_str(root, "type") : "", "custom") == 0;
    yyjson_doc_free(doc);
    return custom;
}

static bool agent_store_is_custom(const cu_impl *o) {
    const char *json =
        o->ctx && o->ctx->cursor_config ? o->ctx->cursor_config->agent_options_json : NULL;
    yyjson_doc *doc = json ? jparse(json, strlen(json)) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *local = jget(root, "local");
    yyjson_val *store = jget(local, "store");
    const char *type = jget_str(store, "type");
    bool custom = type && strcmp(type, "custom") == 0;
    yyjson_doc_free(doc);
    return custom;
}
#endif

static char *configured_custom_tools(const cu_impl *o) {
    const char *json =
        o->ctx && o->ctx->cursor_config ? o->ctx->cursor_config->agent_options_json : NULL;
    yyjson_doc *doc = json ? jparse(json, strlen(json)) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *local = jget(root, "local");
    yyjson_val *tools = jget(local, "customTools");
    if (!tools) tools = jget(local, "custom_tools");
    char *out = yyjson_is_obj(tools) ? jwrite_val(tools) : NULL;
    yyjson_doc_free(doc);
    return out;
}

static char *strip_persistent_agent_store(char *json, char *err, size_t errlen) {
    if (!json) return NULL;
    yyjson_doc *parsed = jparse(json, strlen(json));
    free(json);
    yyjson_mut_doc *doc = parsed ? yyjson_doc_mut_copy(parsed, jallocator()) : NULL;
    yyjson_doc_free(parsed);
    if (!doc) {
        snprintf(err, errlen, "cursor: could not disable the persistent store");
        return NULL;
    }
    yyjson_mut_val *local = yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "local");
    if (yyjson_mut_is_obj(local)) yyjson_mut_obj_remove_key(local, "store");
    char *out = jwrite(doc);
    yyjson_mut_doc_free(doc);
    if (!out) snprintf(err, errlen, "cursor: could not serialize ephemeral agent options");
    return out;
}

static const char *runtime_name(const cu_impl *o) {
    if (!o->ctx || !o->ctx->cursor_config) return "auto";
    switch (o->ctx->cursor_config->runtime) {
    case TNY_CURSOR_RUNTIME_LOCAL: return "local";
    case TNY_CURSOR_RUNTIME_CLOUD: return "cloud";
    default: {
        const char *json = o->ctx->cursor_config->agent_options_json;
        yyjson_doc *doc = json ? jparse(json, strlen(json)) : NULL;
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        bool cloud = yyjson_is_obj(jget(root, "cloud"));
        yyjson_doc_free(doc);
        /* Auto with no explicit runtime follows sdk.v1's local default. */
        return cloud ? "cloud" : "local";
    }
    }
}

static const char *parse_session_pointer(cu_impl *o, const char *ptr, char *err, size_t errlen) {
    static const char prefix[] = "cursor-sdk.v1:";
    if (!ptr || !*ptr || !str_starts(ptr, prefix)) return ptr;
    yyjson_doc *doc = jparse(ptr + sizeof prefix - 1, strlen(ptr + sizeof prefix - 1));
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *agent = jget_str(root, "agent_id");
    const char *run = jget_str(root, "run_id");
    const char *offset = jget_str(root, "after_offset");
    const char *runtime = jget_str(root, "runtime");
    if (!yyjson_is_obj(root) || !agent || !*agent) {
        yyjson_doc_free(doc);
        snprintf(err, errlen, "cursor: malformed sdk.v1 session pointer");
        return NULL;
    }
    if (runtime && strcmp(runtime, runtime_name(o)) != 0) {
        yyjson_doc_free(doc);
        snprintf(err, errlen, "cursor: session runtime is %s, configured runtime is %s", runtime,
                 runtime_name(o));
        return NULL;
    }
    free(o->run_id);
    o->run_id = run && *run ? xstrdup(run) : NULL;
    free(o->observe_offset);
    o->observe_offset = offset && *offset ? xstrdup(offset) : NULL;
    free(o->agent_id);
    o->agent_id = xstrdup(agent);
    yyjson_doc_free(doc);
    return o->agent_id;
}

/* Local agents must name a model; ask the catalog when the user did not. */
static int resolve_model(cu_impl *o, char *err, size_t errlen) {
    if (o->model) return 0;
    if (o->ctx->model && *o->ctx->model) {
        o->model = xstrdup(o->ctx->model);
        return 0;
    }
    buf_t body;
    buf_init(&body);
    buf_appends(&body, "{");
    if (o->api_key) {
        /* catalog RPCs take CursorRequestOptions: {"options":{"apiKey":…}} */
        buf_appends(&body, "\"options\":{\"apiKey\":");
        jescape(&body, o->api_key);
        buf_appends(&body, "}");
    }
    buf_appends(&body, "}");
    char *res = rpc(o, CURSOR_SDK_RPC_LIST_MODELS, body.data, err, errlen);
    buf_free(&body);
    if (!res) {
        snprintf(err, errlen, "cannot pick a model: ListModels failed; pass --model");
        return -1;
    }
    yyjson_doc *doc = jparse(res, strlen(res));
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *arr = jget(root, "items"); /* ListModelsResponse.items */
    if (!arr) arr = jget(root, "models");
    if (!arr) arr = jget(root, "data");
    yyjson_val *first = yyjson_arr_get_first(arr);
    const char *id = NULL;
    if (first && yyjson_is_str(first)) id = yyjson_get_str(first);
    else if (first) {
        id = jget_str(first, "id");
        if (!id) id = jget_str(first, "name");
        if (!id) id = jget_str(first, "model");
    }
    if (id && *id) o->model = xstrdup(id);
    yyjson_doc_free(doc);
    free(res);
    if (!o->model) {
        snprintf(err, errlen, "the bridge listed no models; pass --model");
        return -1;
    }
    return 0;
}

static void effort_clear(cu_impl *o) {
    free(o->effort_for);
    o->effort_for = NULL;
    free(o->effort_param);
    o->effort_param = NULL;
    free(o->effort_value);
    o->effort_value = NULL;
    free(o->effort_note);
    o->effort_note = NULL;
}

/* Resolve ctx->reasoning_effort against the catalog for o->model: find the
 * ModelParameterDefinition whose id names an effort and pick an allowed
 * value (the user's token verbatim, else the canonical mapping). Runs once
 * per distinct setting; called from create_or_resume (possibly the TUI
 * pre-warm thread, so no output here — degradations land in effort_note and
 * are emitted from cu_send). Best effort by design: a failure never blocks
 * the turn, it just runs at the model's default effort. */
static void resolve_effort(cu_impl *o) {
    const char *want = o->ctx->reasoning_effort;
    if (!want || !*want) {
        if (o->effort_for) effort_clear(o);
        return;
    }
    if (o->effort_for && strcmp(o->effort_for, want) == 0) return; /* cached */
    effort_clear(o);
    o->effort_for = xstrdup(want);
    const char *wire = tny_effort_wire(TNY_BK_CURSOR, want);

    buf_t body;
    buf_init(&body);
    buf_appends(&body, "{");
    if (o->api_key) {
        buf_appends(&body, "\"options\":{\"apiKey\":");
        jescape(&body, o->api_key);
        buf_appends(&body, "}");
    }
    buf_appends(&body, "}");
    char err[256];
    char *res = rpc(o, CURSOR_SDK_RPC_LIST_MODELS, body.data, err, sizeof err);
    buf_free(&body);
    yyjson_doc *doc = res ? jparse(res, strlen(res)) : NULL;
    free(res);
    if (!doc) {
        /* catalog unreachable: send the common param id unverified — the
         * bridge drops unknown params silently, so say we guessed */
        o->effort_param = xstrdup("effort");
        o->effort_value = xstrdup(wire);
        buf_t n;
        buf_init(&n);
        buf_appendf(&n, "cursor: ListModels failed; sent effort \"%s\" unverified", wire);
        o->effort_note = buf_detach(&n);
        return;
    }

    yyjson_val *items = jget(yyjson_doc_get_root(doc), "items");
    yyjson_val *def = NULL; /* effort ModelParameterDefinition for o->model */
    size_t idx, max;
    yyjson_val *m;
    if (items && yyjson_is_arr(items)) {
        yyjson_arr_foreach(items, idx, max, m) {
            const char *id = jget_str(m, "id");
            if (!id || !o->model || strcmp(id, o->model) != 0) continue;
            yyjson_val *params = jget(m, "parameters");
            size_t pi, pmax;
            yyjson_val *p;
            if (params && yyjson_is_arr(params)) {
                yyjson_arr_foreach(params, pi, pmax, p) {
                    const char *pid = jget_str(p, "id");
                    if (pid && strstr(pid, "effort")) {
                        def = p;
                        break;
                    }
                }
            }
            break;
        }
    }
    if (!def) {
        buf_t n;
        buf_init(&n);
        buf_appendf(&n,
                    "cursor: model %s advertises no reasoning-effort "
                    "parameter; --effort was ignored",
                    o->model ? o->model : "?");
        o->effort_note = buf_detach(&n);
        yyjson_doc_free(doc);
        return;
    }

    yyjson_val *values = jget(def, "values");
    const char *picked = NULL;
    for (int pass = 0; pass < 2 && !picked; pass++) {
        const char *cand = pass == 0 ? want : wire;
        size_t vi, vmax;
        yyjson_val *v;
        if (values && yyjson_is_arr(values)) {
            yyjson_arr_foreach(values, vi, vmax, v) {
                const char *val = jget_str(v, "value");
                if (val && strcmp(val, cand) == 0) {
                    picked = val;
                    break;
                }
            }
        }
    }
    if (picked) {
        o->effort_param = xstrdup(jget_str(def, "id"));
        o->effort_value = xstrdup(picked);
    } else {
        buf_t n;
        buf_init(&n);
        buf_appendf(&n,
                    "cursor: model %s has no effort \"%s\" (available:", o->model ? o->model : "?",
                    want);
        size_t vi, vmax;
        yyjson_val *v;
        if (values && yyjson_is_arr(values)) {
            yyjson_arr_foreach(values, vi, vmax, v) {
                const char *val = jget_str(v, "value");
                if (val) buf_appendf(&n, " %s", val);
            }
        }
        buf_appends(&n, "); running at the model default");
        o->effort_note = buf_detach(&n);
    }
    yyjson_doc_free(doc);
}

/* Normalized model catalog: ListModelsResponse.items ->
 * [{"id","name","efforts":[…]},…]. */
static int cu_list_models(tny_backend *b, char **out, char *e, size_t el) {
    cu_impl *o = b->impl;
    buf_t body;
    buf_init(&body);
    buf_appends(&body, "{");
    if (o->api_key) {
        buf_appends(&body, "\"options\":{\"apiKey\":");
        jescape(&body, o->api_key);
        buf_appends(&body, "}");
    }
    buf_appends(&body, "}");
    char *res = rpc(o, CURSOR_SDK_RPC_LIST_MODELS, body.data, e, el);
    buf_free(&body);
    if (!res) return -1;
    yyjson_doc *doc = jparse(res, strlen(res));
    free(res);
    if (!doc) {
        snprintf(e, el, "cursor: ListModels returned junk");
        return -1;
    }
    yyjson_val *arr = jget(yyjson_doc_get_root(doc), "items");
    buf_t j;
    buf_init(&j);
    buf_appends(&j, "[");
    if (arr && yyjson_is_arr(arr)) {
        size_t idx, max;
        yyjson_val *m;
        bool first = true;
        yyjson_arr_foreach(arr, idx, max, m) {
            const char *id = jget_str(m, "id");
            if (!id || !*id) continue;
            if (!first) buf_appends(&j, ",");
            first = false;
            buf_appends(&j, "{\"id\":");
            jescape(&j, id);
            const char *nm = jget_str(m, "displayName");
            if (nm) {
                buf_appends(&j, ",\"name\":");
                jescape(&j, nm);
            }
            /* advertise effort levels from the model's parameter catalog */
            yyjson_val *params = jget(m, "parameters");
            size_t pi, pmax;
            yyjson_val *p;
            if (params && yyjson_is_arr(params)) {
                yyjson_arr_foreach(params, pi, pmax, p) {
                    const char *pid = jget_str(p, "id");
                    yyjson_val *values = jget(p, "values");
                    if (!pid || !strstr(pid, "effort") || !values || !yyjson_is_arr(values) ||
                        !yyjson_arr_size(values))
                        continue;
                    buf_appends(&j, ",\"efforts\":[");
                    size_t vi, vmax;
                    yyjson_val *v;
                    bool vfirst = true;
                    yyjson_arr_foreach(values, vi, vmax, v) {
                        const char *val = jget_str(v, "value");
                        if (!val) continue;
                        if (!vfirst) buf_appends(&j, ",");
                        vfirst = false;
                        jescape(&j, val);
                    }
                    buf_appends(&j, "]");
                    break;
                }
            }
            buf_appends(&j, "}");
        }
    }
    buf_appends(&j, "]");
    yyjson_doc_free(doc);
    *out = buf_detach(&j);
    return 0;
}

/* ---------- vtable ---------- */

static int cu_connect(tny_backend *b, char *e, size_t el) {
#ifdef __EMSCRIPTEN__
    (void)b;
    snprintf(e, el, "cursor: conversational sdk.v1 bridge is unavailable in WebAssembly");
    return -1;
#else
    cu_impl *o = b->impl;
    if (o->connected) return 0;
    if (!o->api_key) {
        snprintf(e, el,
                 "no Cursor API key: set CURSOR_API_KEY "
                 "(user or service-account key; Team Admin keys are not supported)");
        return -1;
    }
    tny_cursor_config *cfg = o->ctx->cursor_config;
    if (o->ctx->no_save && strcmp(runtime_name(o), "local") == 0) {
        o->ephemeral_root = cu_ephemeral_root_create(e, el);
        if (!o->ephemeral_root) return -1;
    }
    bool registered_tools =
        o->ctx->custom_tools && custom_tools_active_count(o->ctx->custom_tools) > 0;
    if (registered_tools && cfg && cfg->runtime == TNY_CURSOR_RUNTIME_CLOUD) {
        snprintf(e, el, "cursor: custom tool callbacks are supported only by local agents");
        return -1;
    }
    bool tools = cfg && cfg->tool_callbacks && registered_tools;
    bool requests_custom_store =
        cfg && (json_store_is_custom(cfg->local_store_json) || agent_store_is_custom(o));
    bool custom_store = cfg && !o->ctx->no_save && cfg->store_callbacks && requests_custom_store;
    if (!o->ctx->no_save && requests_custom_store && !cfg->store_callbacks) {
        snprintf(e, el, "cursor: local_store type custom requires callbacks.store=true");
        return -1;
    }
    if (tools || custom_store) {
        cursor_callbacks_options callback_options = {
            .tools = o->ctx->custom_tools,
            .state_dir = cfg && cfg->state_root ? cfg->state_root : o->ctx->tny_dir,
            .enable_tools = tools,
            .enable_store = custom_store,
            .allow_sensitive_tools = o->ctx->perm_mode == TNY_MODE_YOLO,
        };
        o->callbacks = cursor_callbacks_start(&callback_options, e, el);
        if (!o->callbacks) return -1;
    }
    cursor_bridge_launch_options launch = {
        o->ephemeral_root         ? o->ephemeral_root
        : cfg && !o->ctx->no_save ? cfg->state_root
                                  : NULL,
        cfg && !o->ctx->no_save ? cfg->local_store_json : NULL,
        custom_store ? cursor_callbacks_url(o->callbacks) : NULL,
        custom_store ? cursor_callbacks_token(o->callbacks) : NULL,
    };
    if (cursor_bridge_spawn(&o->bridge, o->ctx, o->api_key, &launch, READY_TIMEOUT_MS, e, el) !=
        0) {
        cursor_callbacks_destroy(&o->callbacks);
        cu_ephemeral_root_remove(&o->ephemeral_root);
        return -1;
    }
    cursor_sdk_client_init(&o->sdk, o->bridge.info.url, o->bridge.token);
    cursor_sdk_error sdk_error;
    cursor_sdk_error_init(&sdk_error);
    if (cursor_sdk_client_negotiate(&o->sdk, RPC_TIMEOUT_MS, &sdk_error, e, el) != 0) {
        cursor_sdk_error_free(&sdk_error);
        cursor_sdk_client_close(&o->sdk);
        cursor_bridge_stop(&o->bridge, 2000);
        cursor_callbacks_destroy(&o->callbacks);
        cu_ephemeral_root_remove(&o->ephemeral_root);
        return -1;
    }
    cursor_sdk_error_free(&sdk_error);
    if (tools) {
        buf_t request;
        buf_init(&request);
        buf_appends(&request, "{\"url\":");
        jescape(&request, cursor_callbacks_url(o->callbacks));
        buf_appends(&request, ",\"authToken\":");
        jescape(&request, cursor_callbacks_token(o->callbacks));
        buf_appends(&request, "}");
        char *response = rpc(o, CURSOR_SDK_RPC_SET_TOOL_CALLBACK, request.data, e, el);
        buf_free(&request);
        if (!response) {
            cursor_sdk_client_close(&o->sdk);
            cursor_bridge_stop(&o->bridge, 2000);
            cursor_callbacks_destroy(&o->callbacks);
            cu_ephemeral_root_remove(&o->ephemeral_root);
            return -1;
        }
        free(response);
    }
    o->connected = true;
    return 0;
#endif
}

static void cu_disconnect(tny_backend *b) {
    cu_impl *o = b->impl;
    cursor_sdk_stream_stop(&o->sdk);
    if (o->connected) {
        char err[256];
        char body[64];
        snprintf(body, sizeof body, "{\"graceSeconds\":%d}", SHUTDOWN_GRACE_S);
        bool pump_store = cursor_callbacks_store_started(o->callbacks) &&
                          cursor_callbacks_blocking_begin(o->callbacks, err, sizeof err) == 0;
        char *res = cursor_sdk_invoke_unary(&o->sdk, CURSOR_SDK_RPC_SHUTDOWN, body,
                                            SHUTDOWN_GRACE_S * 1000, NULL, err, sizeof err);
        if (pump_store) cursor_callbacks_blocking_end(o->callbacks);
        free(res); /* best effort: SIGTERM below is the real guarantee */
    }
    cursor_sdk_client_close(&o->sdk);
    cursor_bridge_stop(&o->bridge, SHUTDOWN_GRACE_S * 1000);
    cursor_callbacks_destroy(&o->callbacks);
    cu_ephemeral_root_remove(&o->ephemeral_root);
    o->connected = false;
}

static int cu_create_or_resume(tny_backend *b, const char *ptr, char *e, size_t el) {
    cu_impl *o = b->impl;
    if (resolve_model(o, e, el) != 0) return -1;
    resolve_effort(o); /* best effort; degradations surface at send time */

    const char *resume_id = parse_session_pointer(o, ptr, e, el);
    if (ptr && *ptr && !resume_id) return -1;
    char *configured_tools = NULL;
    char *tool_definitions = NULL;
    if (cursor_callbacks_tools_started(o->callbacks)) {
        configured_tools = configured_custom_tools(o);
        tool_definitions = cursor_callbacks_tool_definitions(o->callbacks, configured_tools);
        free(configured_tools);
        if (!tool_definitions) {
            snprintf(e, el, "cursor: could not construct custom-tool definitions");
            return -1;
        }
    }
    char *options = cursor_options_agent_json(o->ctx, o->api_key, tool_definitions, e, el);
    free(tool_definitions);
    if (o->ctx->no_save) options = strip_persistent_agent_store(options, e, el);
    options = overlay_model_selection(o, options, true, e, el);
    if (!options) return -1;

    buf_t body;
    buf_init(&body);
    cursor_sdk_rpc_id method = CURSOR_SDK_RPC_CREATE_AGENT;
    if (resume_id && *resume_id) {
        method = CURSOR_SDK_RPC_RESUME_AGENT;
        buf_appends(&body, "{\"agentId\":");
        jescape(&body, resume_id);
        buf_appends(&body, ",\"options\":");
    } else {
        buf_appends(&body, "{\"options\":");
    }
    buf_appends(&body, options);
    buf_appends(&body, "}");
    free(options);

    bool pump_store = cursor_callbacks_store_started(o->callbacks);
    if (pump_store && cursor_callbacks_blocking_begin(o->callbacks, e, el) != 0) {
        buf_free(&body);
        return -1;
    }
    char *res = rpc(o, method, body.data, e, el);
    if (pump_store) cursor_callbacks_blocking_end(o->callbacks);
    buf_free(&body);
    if (!res) return -1;
    char *fallback_id = resume_id && *resume_id ? xstrdup(resume_id) : NULL;
    free(o->agent_id);
    o->agent_id = parse_agent_id(res);
    free(res);
    if (!o->agent_id) {
        o->agent_id = fallback_id;
        fallback_id = NULL;
    }
    free(fallback_id);
    if (!o->agent_id) {
        snprintf(e, el, "%s returned no agent id",
                 method == CURSOR_SDK_RPC_RESUME_AGENT ? "ResumeAgent" : "CreateAgent");
        return -1;
    }
    return 0;
}

static char *cu_session_pointer(tny_backend *b) {
    cu_impl *o = b->impl;
    if (!o->agent_id) return NULL;
    buf_t p;
    buf_init(&p);
    buf_appends(&p, "cursor-sdk.v1:{\"agent_id\":");
    jescape(&p, o->agent_id);
    if (o->run_id) {
        buf_appends(&p, ",\"run_id\":");
        jescape(&p, o->run_id);
    }
    if (o->observe_offset) {
        buf_appends(&p, ",\"after_offset\":");
        jescape(&p, o->observe_offset);
    }
    buf_appends(&p, ",\"runtime\":");
    jescape(&p, runtime_name(o));
    buf_appends(&p, "}");
    return buf_detach(&p);
}

static int cu_send(tny_backend *b, const char *prompt, const char **images, tny_backend_event_cb cb,
                   void *ud, char *errbuf, size_t errlen) {
    cu_impl *o = b->impl;
    /* report the model that actually ran (`ask --json`, session meta) —
     * ctx is written here on the caller's thread, never from
     * create_or_resume, which may run on the TUI pre-warm thread */
    if (o->model && (!o->ctx->model || strcmp(o->ctx->model, o->model) != 0)) {
        free(o->ctx->model);
        o->ctx->model = xstrdup(o->model);
    }
    o->cb = cb;
    o->ud = ud;
    if (!o->agent_id) {
        snprintf(errbuf, errlen, "no cursor agent: CreateAgent did not run");
        return -1;
    }
    o->active = true;
    o->ended = o->got_text = o->saw_error = o->usage_sent = false;
    o->in_tok = o->out_tok = 0;
    o->cost = 0;
    o->has_cost = false;
    o->cancel_requested = false;
    o->cancel_sent = false;
    o->cancel_attempted = false;
    o->saw_terminal_result = false;
    o->saw_done = false;
    o->text_source = CU_TEXT_NONE;
    o->stream_kind = CU_STREAM_NONE;
    free(o->send_hashes);
    o->send_hashes = NULL;
    o->send_hash_count = o->send_hash_capacity = o->replay_index = 0;
    o->observe_replay = false;
    o->observe_retry_pending = false;
    o->observe_no_progress_attempts = 0;
    o->observe_retry_at_ms = 0;
    buf_clear(&o->last_status);
    buf_clear(&o->last_tool_start);
    free(o->run_id);
    o->run_id = NULL;
    free(o->observe_offset);
    o->observe_offset = NULL;
    free(o->observe_progress_offset);
    o->observe_progress_offset = NULL;

    /* /effort mid-conversation: re-resolve and ride the params on
     * SendOptions.model, so the change applies without a new agent */
    resolve_effort(o);
    if (o->effort_note) {
        cu_emit_text(o, TNY_EV_STATUS, o->effort_note, strlen(o->effort_note));
        free(o->effort_note);
        o->effort_note = NULL;
    }

    char *options = cursor_options_send_json(o->ctx, true, errbuf, errlen);
    bool model_override = configured_send_model(o) || (o->effort_param && o->effort_value) ||
                          (o->ctx->service_tier && *o->ctx->service_tier);
    options = overlay_model_selection(o, options, model_override, errbuf, errlen);
    if (!options) {
        o->active = false;
        return -1;
    }

    /* SendRequest: agent id + bounded UserMessage image data + the complete
     * configured SendOptions object. */
    buf_t body;
    buf_init(&body);
    buf_appends(&body, "{\"agentId\":");
    jescape(&body, o->agent_id);
    buf_appends(&body, ",\"message\":{\"text\":");
    jescape(&body, prompt);
    if (cu_append_images(&body, images, errbuf, errlen) != 0) {
        free(options);
        buf_free(&body);
        o->active = false;
        return -1;
    }
    buf_appends(&body, "},\"options\":");
    buf_appends(&body, options);
    buf_appends(&body, "}");
    free(options);
    int rc = cursor_sdk_invoke_stream(&o->sdk, CURSOR_SDK_RPC_SEND, body.data, errbuf, errlen);
    buf_free(&body);
    if (rc != 0) {
        o->active = false;
        return -1;
    }
    o->stream_kind = CU_STREAM_SEND;
    /* The bridge runs Cursor's own headless loop; tny runs it yolo by design
     * (docs/adr/0001) — no per-call approvals, and no warning about it. */
    return 0;
}

int cu_start_observe(cu_impl *o, bool from_start, char *err, size_t errlen) {
    if (!o->run_id || !*o->run_id) {
        snprintf(err, errlen, "cursor: run stream dropped before it identified the run");
        return -1;
    }
    buf_t body;
    buf_init(&body);
    buf_appends(&body, "{\"runId\":");
    jescape(&body, o->run_id);
    if (!from_start && o->observe_offset) {
        buf_appends(&body, ",\"afterOffset\":");
        jescape(&body, o->observe_offset);
    }
    buf_appends(&body, "}");
    int rc = cursor_sdk_invoke_stream(&o->sdk, CURSOR_SDK_RPC_OBSERVE_RUN, body.data, err, errlen);
    buf_free(&body);
    if (rc == 0) {
        free(o->observe_progress_offset);
        o->observe_progress_offset = o->observe_offset ? xstrdup(o->observe_offset) : NULL;
        o->observe_retry_pending = false;
        o->observe_retry_at_ms = 0;
        o->saw_done = false;
        if (from_start) {
            o->observe_replay = true;
            o->replay_index = 0;
        }
        o->stream_kind = CU_STREAM_OBSERVE;
    }
    return rc;
}

int cu_send_cancel(cu_impl *o, char *err, size_t errlen) {
    if (o->cancel_attempted || !o->cancel_requested || !o->run_id) return 0;
    o->cancel_attempted = true;
    buf_t body;
    buf_init(&body);
    buf_appends(&body, "{\"runId\":");
    jescape(&body, o->run_id);
    if (o->agent_id) {
        buf_appends(&body, ",\"agentId\":");
        jescape(&body, o->agent_id);
    }
    buf_appends(&body, "}");
    bool pump_store = cursor_callbacks_store_started(o->callbacks);
    if (pump_store && cursor_callbacks_blocking_begin(o->callbacks, err, errlen) != 0) {
        buf_free(&body);
        return -1;
    }
    char *res = rpc(o, CURSOR_SDK_RPC_CANCEL_RUN, body.data, err, errlen);
    if (pump_store) cursor_callbacks_blocking_end(o->callbacks);
    buf_free(&body);
    if (!res) return -1;
    free(res);
    o->cancel_sent = true;
    return 0;
}

static void cu_cancel(tny_backend *b) {
    cu_impl *o = b->impl;
    if (!o->active) return;
    o->cancel_requested = true;
    char err[256];
    if (o->run_id && cu_send_cancel(o, err, sizeof err) != 0) {
        cu_emit_text(o, TNY_EV_STATUS, err, strlen(err));
        if (!o->ctx->library_mode && tny_debug())
            fprintf(stderr, "tny: cursor: CancelRun failed: %s\n", err);
    } else if (!o->run_id) {
        static const char waiting[] = "cursor: waiting for the run id before cancelling";
        cu_emit_text(o, TNY_EV_STATUS, waiting, sizeof waiting - 1);
    }
    /* Keep Send/ObserveRun open. The terminal CANCELLED result is the only
     * authoritative completion signal. */
}

static void cu_respond_permission(tny_backend *b, const char *id, tny_perm_decision d) {
    (void)b;
    (void)id;
    (void)d;
    /* The bridge is headless: there is no Allow/Deny RPC
     * (docs/backends/cursor-bridge.md). Per-call approvals are ACP. */
}

static int cu_pollfds(tny_backend *b, struct pollfd *fds, int max) {
    cu_impl *o = b->impl;
    int n = 0;
    int sfd = cursor_sdk_stream_fd(&o->sdk);
    if (sfd >= 0 && n < max) {
        fds[n].fd = sfd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    if (o->bridge.err_fd >= 0 && n < max) {
        fds[n].fd = o->bridge.err_fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    if (o->callbacks && n < max) n += cursor_callbacks_pollfds(o->callbacks, fds + n, max - n);
    return n;
}

static int cu_poll_timeout(tny_backend *b) {
    cu_impl *o = b->impl;
    if (!o->observe_retry_pending) return -1;
    int64_t remaining = o->observe_retry_at_ms - now_ms();
    if (remaining <= 0) return 0;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

static void cu_note_observe_progress(cu_impl *o) {
    if (o->stream_kind != CU_STREAM_OBSERVE || !o->observe_offset) return;
    if (o->observe_progress_offset && strcmp(o->observe_offset, o->observe_progress_offset) == 0)
        return;
    free(o->observe_progress_offset);
    o->observe_progress_offset = xstrdup(o->observe_offset);
    o->observe_no_progress_attempts = 0;
}

static int cu_dispatch(tny_backend *b, struct pollfd *fds, int n) {
    cu_impl *o = b->impl;
    if (o->callbacks && cursor_callbacks_dispatch(o->callbacks, fds, n) != 0) {
        static const char error[] = "cursor: callback server failed";
        if (!o->ended) {
            cu_emit_text(o, TNY_EV_ERROR, error, sizeof error - 1);
            cu_end_turn(o, TNY_STOP_ERROR);
        }
        return -1;
    }
    cursor_bridge_pump(&o->bridge);
    if (o->observe_retry_pending) {
        if (now_ms() < o->observe_retry_at_ms) return 0;
        o->observe_retry_pending = false;
        char recover_err[300];
        if (cu_start_observe(o, false, recover_err, sizeof recover_err) == 0) {
            static const char resumed[] = "cursor: recovering the durable run stream";
            cu_emit_text(o, TNY_EV_STATUS, resumed, sizeof resumed - 1);
            return 0;
        }
        buf_t message;
        buf_init(&message);
        buf_appendf(&message, "cursor stream closed; recovery failed: %s", recover_err);
        cu_emit_text(o, TNY_EV_ERROR, message.data, message.len);
        buf_free(&message);
        cu_end_turn(o, TNY_STOP_ERROR);
        return -1;
    }
    if (o->sdk.stream.state == CS_IDLE) return 0;

    char err[300];
    cursor_sdk_error sdk_error;
    cursor_sdk_error_init(&sdk_error);
    int rc = cursor_sdk_stream_pump(&o->sdk, cu_on_frame, o, &sdk_error, err, sizeof err);
    cursor_sdk_error_free(&sdk_error);
    cu_note_observe_progress(o);

    if (o->saw_terminal_result) {
        o->observe_no_progress_attempts = 0;
        o->observe_retry_pending = false;
        o->observe_retry_at_ms = 0;
    }

    if (o->cancel_requested && !o->cancel_attempted && o->run_id) {
        char cancel_err[300];
        if (cu_send_cancel(o, cancel_err, sizeof cancel_err) != 0)
            cu_emit_text(o, TNY_EV_STATUS, cancel_err, strlen(cancel_err));
    }

    if (rc != 0 && !o->ended) {
        cu_stream_kind dropped = o->stream_kind;
        cursor_sdk_stream_stop(&o->sdk);
        if (dropped == CU_STREAM_OBSERVE && o->saw_done) {
            static const char missing[] =
                "cursor: ObserveRun ended without a terminal RunStreamResult";
            cu_emit_text(o, TNY_EV_ERROR, missing, sizeof missing - 1);
            cu_end_turn(o, TNY_STOP_ERROR);
            return -1;
        }
        bool from_start = dropped == CU_STREAM_SEND;
        if (!from_start) {
            if (o->observe_no_progress_attempts >= OBSERVE_RECOVERY_MAX_NO_PROGRESS) {
                static const char exhausted[] =
                    "cursor: ObserveRun repeatedly closed without durable progress";
                cu_emit_text(o, TNY_EV_ERROR, exhausted, sizeof exhausted - 1);
                cu_end_turn(o, TNY_STOP_ERROR);
                return -1;
            }
            o->observe_no_progress_attempts++;
            unsigned shift = o->observe_no_progress_attempts - 1u;
            o->observe_retry_at_ms = now_ms() + ((int64_t)OBSERVE_RECOVERY_BASE_DELAY_MS << shift);
            o->observe_retry_pending = true;
            return 0;
        }
        char recover_err[300];
        if (cu_start_observe(o, from_start, recover_err, sizeof recover_err) == 0) {
            static const char resumed[] = "cursor: recovering the durable run stream";
            cu_emit_text(o, TNY_EV_STATUS, resumed, sizeof resumed - 1);
            return 0;
        }
        buf_t message;
        buf_init(&message);
        buf_appendf(&message, "%s; recovery failed: %s", err[0] ? err : "cursor stream closed",
                    recover_err);
        cu_emit_text(o, TNY_EV_ERROR, message.data, message.len);
        buf_free(&message);
        cu_end_turn(o, TNY_STOP_ERROR);
        return -1;
    }
    return 0;
}

/* ---------- doctor ---------- */

static bool bin_on_path(const char *bin) {
    if (strchr(bin, '/')) return access(bin, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) return false;
    char *dup = xstrdup(path);
    bool found = false;
    for (char *p = strtok(dup, ":"); p && !found; p = strtok(NULL, ":")) {
        char *full = path_join(p, bin);
        if (access(full, X_OK) == 0) found = true;
        free(full);
    }
    free(dup);
    return found;
}

static int cu_doctor(struct tny_ctx *ctx, char *line, size_t linelen) {
    const char *bin = ctx->bridge_bin && *ctx->bridge_bin ? ctx->bridge_bin : "cursor-sdk-bridge";
    if (!bin_on_path(bin)) {
        snprintf(line, linelen,
                 "cursor: %s not found (set CURSOR_SDK_BRIDGE_BIN "
                 "or --bridge-bin)",
                 bin);
        return 1;
    }
    const char *key = getenv("CURSOR_API_KEY");
    if (!key || !*key) {
        snprintf(line, linelen, "cursor: %s found, but CURSOR_API_KEY is not set", bin);
        return 1;
    }
    if (getenv("TNY_DOCTOR_NO_SPAWN")) {
        snprintf(line, linelen, "cursor: %s found, key present (probe skipped)", bin);
        return 0;
    }

    cursor_bridge bp;
    cursor_bridge_init(&bp);
    char err[300];
    if (cursor_bridge_spawn(&bp, ctx, key, NULL, 8000, err, sizeof err) != 0) {
        snprintf(line, linelen, "cursor: bridge did not start: %.180s", err);
        return 1;
    }
    cursor_sdk_client sdk;
    cursor_sdk_client_init(&sdk, bp.info.url, bp.token);
    cursor_sdk_error sdk_error;
    cursor_sdk_error_init(&sdk_error);
    int rc = cursor_sdk_client_negotiate(&sdk, 5000, &sdk_error, err, sizeof err) == 0 ? 0 : 1;
    if (rc) {
        snprintf(line, linelen, "cursor: bridge sdk.v1 negotiation failed: %.150s", err);
    } else {
        snprintf(line, linelen, "cursor: bridge %s ok (%s, %zu capabilities)",
                 sdk.version.bridge_version, sdk.version.protocol_version,
                 sdk.version.capability_count);
    }
    cursor_sdk_error_free(&sdk_error);
    cursor_sdk_client_close(&sdk);
    cursor_bridge_stop(&bp, 2000);
    return rc;
}

/* ---------- lifecycle ---------- */

static void cu_destroy(tny_backend *b) {
    cu_impl *o = b->impl;
    cu_disconnect(b);
    secure_free(o->api_key);
    secure_zero(o->bridge.token, sizeof o->bridge.token);
    secure_zero(o->bridge.info.auth_token, sizeof o->bridge.info.auth_token);
    secure_zero(o->sdk.rpc.token, sizeof o->sdk.rpc.token);
    secure_zero(o->sdk.stream.token, sizeof o->sdk.stream.token);
    free(o->model);
    free(o->agent_id);
    free(o->run_id);
    free(o->observe_offset);
    free(o->observe_progress_offset);
    free(o->send_hashes);
    cu_ephemeral_root_remove(&o->ephemeral_root);
    effort_clear(o);
    buf_free(&o->last_status);
    buf_free(&o->last_tool_start);
    free(o);
    free(b);
}

tny_backend *tny_backend_cursor_new(struct tny_ctx *ctx) {
    tny_backend *b = calloc(1, sizeof *b);
    cu_impl *o = calloc(1, sizeof *o);
    if (!b || !o) {
        free(b);
        free(o);
        return NULL;
    }
    o->ctx = ctx;
    cursor_bridge_init(&o->bridge);
    cursor_sdk_client_init(&o->sdk, "", "");
    buf_init(&o->last_status);
    buf_init(&o->last_tool_start);
    /* CLI contexts keep the Cursor key in CURSOR_API_KEY so an OpenAI key in
     * ctx can never cross providers. Deterministic libtny contexts explicitly
     * name provider_name=cursor and may carry their copied key in ctx. */
    const char *key = ctx->provider_name && strcmp(ctx->provider_name, "cursor") == 0
                          ? ctx->api_key
                          : getenv("CURSOR_API_KEY");
    if (key && *key) o->api_key = xstrdup(key);

    b->id = TNY_BK_CURSOR;
    b->impl = o;
    b->connect = cu_connect;
    b->disconnect = cu_disconnect;
    b->create_or_resume = cu_create_or_resume;
    b->session_pointer = cu_session_pointer;
    b->send = cu_send;
    b->cancel = cu_cancel;
    b->respond_permission = cu_respond_permission;
    b->pollfds = cu_pollfds;
    b->poll_timeout = cu_poll_timeout;
    b->dispatch = cu_dispatch;
    b->list_models = cu_list_models;
    b->doctor = cu_doctor;
    b->destroy = cu_destroy;
    return b;
}
