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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define READY_TIMEOUT_MS 30000
#define RPC_TIMEOUT_MS   30000
#define SHUTDOWN_GRACE_S 5

/* ---------- events ---------- */

void cu_emit(cu_impl *o, const tny_event *ev) {
    if (o->cb) o->cb(ev, o->ud);
}

void cu_emit_text(cu_impl *o, tny_event_kind k, const char *t, size_t n) {
    tny_event ev = {0};
    ev.kind = k;
    ev.text = t;
    ev.text_len = n;
    cu_emit(o, &ev);
}

void cu_end_turn(cu_impl *o, tny_stop_reason stop) {
    if (o->ended) return;
    o->ended = true;
    o->active = false;
    if (!o->usage_sent && (o->in_tok || o->out_tok)) {
        o->usage_sent = true;
        tny_event u = {0};
        u.kind = TNY_EV_USAGE;
        u.in_tokens = o->in_tok;
        u.out_tokens = o->out_tok;
        cu_emit(o, &u);
    }
    tny_event ev = {0};
    ev.kind = TNY_EV_TURN_END;
    ev.stop = stop;
    cu_emit(o, &ev);
}

/* ---------- request bodies ---------- */

/* ModelSelection ({"id":…,"params":[{id,value}…]}): per-model options such as
 * reasoning effort ride in params; top-level keys are silently dropped. */
static void append_model_selection(cu_impl *o, buf_t *b) {
    buf_appends(b, "{\"id\":");
    jescape(b, o->model);
    if (o->effort_param && o->effort_value) {
        buf_appends(b, ",\"params\":[{\"id\":");
        jescape(b, o->effort_param);
        buf_appends(b, ",\"value\":");
        jescape(b, o->effort_value);
        buf_appends(b, "}]");
    }
    buf_appends(b, "}");
}

/* AgentOptions (proto/sdk/v1/sdk_messages.proto): model is a ModelSelection
 * ({"id":…}), local.cwd carries at most one entry, extra roots go in
 * local.dirs. Local agents need an explicit model and a cwd. */
static void append_options(cu_impl *o, buf_t *b) {
    buf_appends(b, "{");
    if (o->model) {
        buf_appends(b, "\"model\":");
        append_model_selection(o, b);
        buf_appends(b, ",");
    }
    if (o->api_key) {
        buf_appends(b, "\"apiKey\":");
        jescape(b, o->api_key);
        buf_appends(b, ",");
    }
    buf_appends(b, "\"local\":{\"cwd\":[");
    jescape(b, o->ctx->cwd);
    buf_appends(b, "]");
    if (o->ctx->n_extra_dirs) {
        buf_appends(b, ",\"dirs\":[");
        for (int i = 0; i < o->ctx->n_extra_dirs; i++) {
            if (i) buf_appends(b, ",");
            jescape(b, o->ctx->extra_dirs[i]);
        }
        buf_appends(b, "]");
    }
    buf_appends(b, "}}");
}

static char *rpc(cu_impl *o, const char *svc, const char *method, const char *body,
                 char *err, size_t errlen) {
    return cursor_rpc_unary(&o->rpc, svc, method, body, RPC_TIMEOUT_MS, err, errlen);
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
    char *res = rpc(o, CURSOR_SVC_CURSOR, "ListModels", body.data, err, errlen);
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
    free(o->effort_for);   o->effort_for = NULL;
    free(o->effort_param); o->effort_param = NULL;
    free(o->effort_value); o->effort_value = NULL;
    free(o->effort_note);  o->effort_note = NULL;
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
    char *res = rpc(o, CURSOR_SVC_CURSOR, "ListModels", body.data, err, sizeof err);
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
        buf_appendf(&n, "cursor: ListModels failed; sent effort \"%s\" unverified",
                    wire);
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
                    if (pid && strstr(pid, "effort")) { def = p; break; }
                }
            }
            break;
        }
    }
    if (!def) {
        buf_t n;
        buf_init(&n);
        buf_appendf(&n, "cursor: model %s advertises no reasoning-effort "
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
                if (val && strcmp(val, cand) == 0) { picked = val; break; }
            }
        }
    }
    if (picked) {
        o->effort_param = xstrdup(jget_str(def, "id"));
        o->effort_value = xstrdup(picked);
    } else {
        buf_t n;
        buf_init(&n);
        buf_appendf(&n, "cursor: model %s has no effort \"%s\" (available:",
                    o->model ? o->model : "?", want);
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
    char *res = rpc(o, CURSOR_SVC_CURSOR, "ListModels", body.data, e, el);
    buf_free(&body);
    if (!res) return -1;
    yyjson_doc *doc = jparse(res, strlen(res));
    free(res);
    if (!doc) { snprintf(e, el, "cursor: ListModels returned junk"); return -1; }
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
            if (nm) { buf_appends(&j, ",\"name\":"); jescape(&j, nm); }
            /* advertise effort levels from the model's parameter catalog */
            yyjson_val *params = jget(m, "parameters");
            size_t pi, pmax;
            yyjson_val *p;
            if (params && yyjson_is_arr(params)) {
                yyjson_arr_foreach(params, pi, pmax, p) {
                    const char *pid = jget_str(p, "id");
                    yyjson_val *values = jget(p, "values");
                    if (!pid || !strstr(pid, "effort") || !values ||
                        !yyjson_is_arr(values) || !yyjson_arr_size(values))
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
    cu_impl *o = b->impl;
    if (o->connected) return 0;
    if (!o->api_key) {
        snprintf(e, el, "no Cursor API key: set CURSOR_API_KEY "
                        "(user or service-account key; Team Admin keys are not supported)");
        return -1;
    }
    if (cursor_bridge_spawn(&o->bridge, o->ctx, o->api_key, READY_TIMEOUT_MS, e, el) != 0)
        return -1;
    cursor_rpc_init(&o->rpc, o->bridge.info.url, o->bridge.token);
    cursor_stream_init(&o->stream, o->bridge.info.url, o->bridge.token);

    char *pong = rpc(o, CURSOR_SVC_CONTROL, "Ping", "{}", e, el);
    if (!pong) {
        cursor_rpc_close(&o->rpc);
        cursor_bridge_stop(&o->bridge, 2000);
        return -1;
    }
    free(pong);
    o->connected = true;
    return 0;
}

static void cu_disconnect(tny_backend *b) {
    cu_impl *o = b->impl;
    cursor_stream_stop(&o->stream);
    if (o->connected) {
        char err[256];
        char body[64];
        snprintf(body, sizeof body, "{\"graceSeconds\":%d}", SHUTDOWN_GRACE_S);
        char *res = cursor_rpc_unary(&o->rpc, CURSOR_SVC_CONTROL, "Shutdown", body,
                                     SHUTDOWN_GRACE_S * 1000, err, sizeof err);
        free(res); /* best effort: SIGTERM below is the real guarantee */
    }
    cursor_rpc_close(&o->rpc);
    cursor_bridge_stop(&o->bridge, SHUTDOWN_GRACE_S * 1000);
    o->connected = false;
}

static int cu_create_or_resume(tny_backend *b, const char *ptr, char *e, size_t el) {
    cu_impl *o = b->impl;
    if (resolve_model(o, e, el) != 0) return -1;
    resolve_effort(o); /* best effort; degradations surface at send time */

    buf_t body;
    buf_init(&body);
    const char *method = "CreateAgent";
    if (ptr && *ptr) {
        method = "ResumeAgent";
        buf_appends(&body, "{\"agentId\":");
        jescape(&body, ptr);
        buf_appends(&body, ",\"options\":");
    } else {
        buf_appends(&body, "{\"options\":");
    }
    append_options(o, &body);
    buf_appends(&body, "}");

    char *res = rpc(o, CURSOR_SVC_AGENT, method, body.data, e, el);
    buf_free(&body);
    if (!res) return -1;
    free(o->agent_id);
    o->agent_id = parse_agent_id(res);
    free(res);
    if (!o->agent_id && ptr && *ptr) o->agent_id = xstrdup(ptr);
    if (!o->agent_id) {
        snprintf(e, el, "%s returned no agent id", method);
        return -1;
    }
    return 0;
}

static char *cu_session_pointer(tny_backend *b) {
    cu_impl *o = b->impl;
    return o->agent_id ? xstrdup(o->agent_id) : NULL;
}

static int cu_send(tny_backend *b, const char *prompt, const char **images,
                   tny_event_cb cb, void *ud, char *errbuf, size_t errlen) {
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
    if (images && images[0]) {
        snprintf(errbuf, errlen,
                 "the cursor backend has no image input yet; drop --image "
                 "or use --provider openai");
        return -1;
    }

    o->active = true;
    o->ended = o->got_text = o->saw_error = o->usage_sent = false;
    o->in_tok = o->out_tok = 0;
    buf_clear(&o->last_status);
    free(o->run_id);
    o->run_id = NULL;

    /* /effort mid-conversation: re-resolve and ride the params on
     * SendOptions.model, so the change applies without a new agent */
    resolve_effort(o);
    if (o->effort_note) {
        cu_emit_text(o, TNY_EV_STATUS, o->effort_note, strlen(o->effort_note));
        free(o->effort_note);
        o->effort_note = NULL;
    }

    /* SendRequest: agent id + UserMessage `message` + SendOptions.enable_deltas. */
    buf_t body;
    buf_init(&body);
    buf_appends(&body, "{\"agentId\":");
    jescape(&body, o->agent_id);
    buf_appends(&body, ",\"message\":{\"text\":");
    jescape(&body, prompt);
    buf_appends(&body, "},\"options\":{\"enableDeltas\":true");
    if (o->model && o->effort_param && o->effort_value) {
        buf_appends(&body, ",\"model\":");
        append_model_selection(o, &body);
    }
    buf_appends(&body, "}}");
    int rc = cursor_stream_start(&o->stream, CURSOR_SVC_AGENT, "Send", body.data,
                                 errbuf, errlen);
    buf_free(&body);
    if (rc != 0) {
        o->active = false;
        return -1;
    }
    /* The bridge runs Cursor's own headless loop; tny runs it yolo by design
     * (docs/adr/0001) — no per-call approvals, and no warning about it. */
    return 0;
}

static void cu_cancel(tny_backend *b) {
    cu_impl *o = b->impl;
    if (!o->active) return;
    if (o->agent_id) {
        buf_t body;
        buf_init(&body);
        buf_appends(&body, "{\"agentId\":");
        jescape(&body, o->agent_id);
        if (o->run_id) {
            buf_appends(&body, ",\"runId\":");
            jescape(&body, o->run_id);
        }
        buf_appends(&body, "}");
        char err[256];
        char *res = rpc(o, CURSOR_SVC_AGENT, "CancelRun", body.data, err, sizeof err);
        if (!res) fprintf(stderr, "tny: cursor: CancelRun failed: %s\n", err);
        free(res);
        buf_free(&body);
    }
    cursor_stream_stop(&o->stream);
    cu_end_turn(o, TNY_STOP_INTERRUPTED);
}

static void cu_respond_permission(tny_backend *b, const char *id, tny_perm_decision d) {
    (void)b; (void)id; (void)d;
    /* The bridge is headless: there is no Allow/Deny RPC
     * (docs/backends/cursor-bridge.md). Per-call approvals are ACP. */
}

static int cu_pollfds(tny_backend *b, struct pollfd *fds, int max) {
    cu_impl *o = b->impl;
    int n = 0;
    int sfd = cursor_stream_fd(&o->stream);
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
    return n;
}

static int cu_dispatch(tny_backend *b, struct pollfd *fds, int n) {
    (void)fds; (void)n;
    cu_impl *o = b->impl;
    cursor_bridge_pump(&o->bridge);
    if (o->stream.state == CS_IDLE) return 0;

    char err[300];
    int rc = cursor_stream_pump(&o->stream, cu_on_frame, o, err, sizeof err);
    if (rc < 0) {
        if (!o->ended) {
            cu_emit_text(o, TNY_EV_ERROR, err, strlen(err));
            cu_end_turn(o, TNY_STOP_ERROR);
        }
        cursor_stream_stop(&o->stream);
        return -1;
    }
    if (rc == 1) {
        cursor_stream_stop(&o->stream);
        if (!o->ended) {
            static const char m[] = "the bridge closed the run stream without a result";
            cu_emit_text(o, TNY_EV_ERROR, m, sizeof m - 1);
            cu_end_turn(o, TNY_STOP_ERROR);
        }
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
    const char *bin = ctx->bridge_bin && *ctx->bridge_bin ? ctx->bridge_bin
                                                          : "cursor-sdk-bridge";
    if (!bin_on_path(bin)) {
        snprintf(line, linelen, "cursor: %s not found (set CURSOR_SDK_BRIDGE_BIN "
                                "or --bridge-bin)", bin);
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
    if (cursor_bridge_spawn(&bp, ctx, key, 8000, err, sizeof err) != 0) {
        snprintf(line, linelen, "cursor: bridge did not start: %.180s", err);
        return 1;
    }
    cursor_rpc r;
    cursor_rpc_init(&r, bp.info.url, bp.token);
    char *res = cursor_rpc_unary(&r, CURSOR_SVC_CONTROL, "GetVersion", "{}", 5000,
                                 err, sizeof err);
    int rc = 1;
    if (!res) {
        snprintf(line, linelen, "cursor: bridge started but GetVersion failed: %.150s", err);
    } else {
        yyjson_doc *doc = jparse(res, strlen(res));
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        const char *bv = jget_str(root, "bridgeVersion");
        if (!bv) bv = jget_str(root, "version");
        const char *sv = jget_str(root, "sdkVersion");
        snprintf(line, linelen, "cursor: bridge %s ok (sdk %s)", bv ? bv : "?",
                 sv ? sv : "?");
        yyjson_doc_free(doc);
        free(res);
        rc = 0;
    }
    cursor_rpc_close(&r);
    cursor_bridge_stop(&bp, 2000);
    return rc;
}

/* ---------- lifecycle ---------- */

static void cu_destroy(tny_backend *b) {
    cu_impl *o = b->impl;
    cu_disconnect(b);
    free(o->api_key);
    free(o->model);
    free(o->agent_id);
    free(o->run_id);
    effort_clear(o);
    buf_free(&o->last_status);
    free(o);
    free(b);
}

tny_backend *tny_backend_cursor_new(struct tny_ctx *ctx) {
    tny_backend *b = calloc(1, sizeof *b);
    cu_impl *o = calloc(1, sizeof *o);
    if (!b || !o) { free(b); free(o); return NULL; }
    o->ctx = ctx;
    cursor_bridge_init(&o->bridge);
    cursor_stream_init(&o->stream, "", "");
    buf_init(&o->last_status);
    /* Deliberately only CURSOR_API_KEY: ctx->api_key is the OpenAI-compatible
     * provider secret and must never be handed to a different service. */
    const char *key = getenv("CURSOR_API_KEY");
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
    b->dispatch = cu_dispatch;
    b->list_models = cu_list_models;
    b->doctor = cu_doctor;
    b->destroy = cu_destroy;
    return b;
}
