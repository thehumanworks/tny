/* openai.c — native OpenAI-compatible backend: Chat Completions SSE + the
 * tny-owned tool loop (docs/backends/openai-compatible.md). */
#include "backends/openai/openai.h"
#include "core/tools.h"
#include "core/image.h"
#include "core/instructions.h"
#include "core/skills.h"
#include "net/net.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>

#define OPENAI_DEFAULT_MODEL "gpt-4.1-mini"
#define MAX_TOOL_CALLS 16

typedef struct {
    char *id;
    char *name;
    buf_t args;
} pending_call;

typedef enum { ST_IDLE, ST_HEADERS, ST_BODY } oa_state;

typedef struct {
    tny_ctx *ctx;
    tools_env env;
    http_conn *conn;
    sse_parser sse;
    oa_state state;

    tny_event_cb cb;
    void *ud;

    buf_t text;             /* assistant text this step */
    pending_call calls[MAX_TOOL_CALLS];
    int  ncalls;
    int  step;
    bool cancelled;
    bool stream_done;       /* saw [DONE] */
    char finish_reason[32];
    int64_t usage_in, usage_out;

    buf_t toolcall_log;     /* JSON array text for ask --json */
    char errbuf[512];
} oa_impl;

/* ---------- helpers ---------- */

static void emit(oa_impl *o, const tny_event *ev) {
    if (o->cb) o->cb(ev, o->ud);
}

static void emit_text(oa_impl *o, tny_event_kind k, const char *t, size_t n) {
    tny_event ev = {0};
    ev.kind = k;
    ev.text = t;
    ev.text_len = n;
    emit(o, &ev);
}

static void emit_turn_end(oa_impl *o, tny_stop_reason stop) {
    o->state = ST_IDLE;
    tny_event ev = {0};
    ev.kind = TNY_EV_TURN_END;
    ev.stop = stop;
    emit(o, &ev);
}

static void calls_reset(oa_impl *o) {
    for (int i = 0; i < o->ncalls; i++) {
        free(o->calls[i].id);
        free(o->calls[i].name);
        buf_free(&o->calls[i].args);
    }
    o->ncalls = 0;
}

static const char *model_of(oa_impl *o) {
    return o->ctx->model ? o->ctx->model : OPENAI_DEFAULT_MODEL;
}

/* Build the request body from the session view. */
static char *build_request(oa_impl *o) {
    tny_session *s = o->env.session;
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"model\":");
    jescape(&b, model_of(o));
    buf_appends(&b, ",\"stream\":true,\"messages\":[");

    /* system preamble */
    buf_t sys;
    buf_init(&sys);
    buf_appendf(&sys,
        "You are tny, a fast coding agent running in a terminal.\n"
        "Primary workspace: %s\n"
        "Use the provided tools to inspect and change the workspace. Prefer "
        "small, verifiable steps. When you are done, answer in Markdown.\n",
        o->ctx->cwd);
    for (int i = 0; i < o->ctx->n_extra_dirs; i++)
        buf_appendf(&sys, "Additional workspace directory: %s\n", o->ctx->extra_dirs[i]);
    instructions_collect(o->ctx, &sys);
    /* skill catalog: names only, lazy bodies */
    int nsk = 0;
    skill_meta *sk = skills_discover(o->ctx, &nsk);
    if (nsk > 0) {
        buf_appends(&sys, "\nAvailable skills (load with the `skill` tool):\n");
        for (int i = 0; i < nsk; i++)
            buf_appendf(&sys, "- %s: %.140s\n", sk[i].name, sk[i].description);
    }
    skills_free(sk, nsk);

    buf_appends(&b, "{\"role\":\"system\",\"content\":");
    jescape(&b, sys.data);
    buf_appends(&b, "}");
    buf_free(&sys);

    /* compacted view */
    const char *summary = NULL;
    int boundary = session_compact_boundary(s, &summary);
    if (summary && boundary > 0) {
        buf_appends(&b, ",{\"role\":\"system\",\"content\":");
        jescape(&b, summary);
        buf_appends(&b, "}");
    }
    yyjson_mut_val *msgs = session_messages(s);
    size_t total = yyjson_mut_arr_size(msgs);
    for (size_t i = (size_t)boundary; i < total; i++) {
        yyjson_mut_val *m = yyjson_mut_arr_get(msgs, i);
        char *mj = yyjson_mut_val_write(m, 0, NULL);
        if (mj) {
            buf_appends(&b, ",");
            buf_appends(&b, mj);
            free(mj);
        }
    }
    buf_appends(&b, "]");

    char *schema = tools_schema_json(&o->env);
    buf_appendf(&b, ",\"tools\":%s,\"tool_choice\":\"auto\"", schema);
    free(schema);
    if (o->ctx->output_schema)
        buf_appendf(&b, ",\"response_format\":%s", o->ctx->output_schema);
    if (o->ctx->max_tokens_field)
        buf_appendf(&b, ",\"%s\":8192", o->ctx->max_tokens_field);
    buf_appends(&b, "}");
    return buf_detach(&b);
}

static int start_post(oa_impl *o, char *errbuf, size_t errlen) {
    char err[256];
    if (!o->conn) {
        o->conn = http_open(o->ctx->base_url, err, sizeof err);
        if (!o->conn) {
            snprintf(errbuf, errlen, "%s", err);
            return -1;
        }
    }
    char *body = build_request(o);
    buf_t auth;
    buf_init(&auth);
    buf_appendf(&auth, "%s: %s%s", o->ctx->auth_header_name,
                o->ctx->auth_header_prefix, o->ctx->api_key ? o->ctx->api_key : "");
    const char *hdrs[] = {
        "Content-Type: application/json",
        "Accept: text/event-stream",
        o->ctx->api_key ? auth.data : NULL,
        NULL
    };
    buf_t path;
    buf_init(&path);
    buf_appendf(&path, "%s/chat/completions", http_prefix(o->conn));
    int rc = http_request(o->conn, "POST", path.data, hdrs, body, strlen(body));
    if (rc != 0) {
        /* stale keep-alive: reopen once */
        http_close(o->conn);
        o->conn = http_open(o->ctx->base_url, err, sizeof err);
        if (o->conn)
            rc = http_request(o->conn, "POST", path.data, hdrs, body, strlen(body));
    }
    buf_free(&path);
    buf_free(&auth);
    free(body);
    if (rc != 0) {
        snprintf(errbuf, errlen, "request to %s failed", o->ctx->base_url);
        return -1;
    }
    o->state = ST_HEADERS;
    o->stream_done = false;
    o->finish_reason[0] = 0;
    buf_clear(&o->text);
    sse_parser_free(&o->sse);
    sse_parser_init(&o->sse);
    return 0;
}

/* ---------- SSE event handling ---------- */

static void on_sse_event(const char *data, size_t len, void *ud) {
    oa_impl *o = ud;
    if ((len == 6 && memcmp(data, "[DONE]", 6) == 0) ||
        (len == 4 && memcmp(data, "DONE", 4) == 0)) {
        o->stream_done = true;
        return;
    }
    yyjson_doc *doc = jparse(data, len);
    if (!doc) return; /* never block the loop on a parse error */
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *usage = jget(root, "usage");
    if (usage) {
        o->usage_in = jget_int(usage, "prompt_tokens", o->usage_in);
        o->usage_out = jget_int(usage, "completion_tokens", o->usage_out);
    }
    yyjson_val *choice = yyjson_arr_get_first(jget(root, "choices"));
    if (!choice) { yyjson_doc_free(doc); return; }
    const char *fr = jget_str(choice, "finish_reason");
    if (fr) snprintf(o->finish_reason, sizeof o->finish_reason, "%s", fr);
    yyjson_val *delta = jget(choice, "delta");
    if (!delta) delta = jget(choice, "message"); /* non-stream fallback */
    const char *content = jget_str(delta, "content");
    if (content && *content) {
        buf_appends(&o->text, content);
        emit_text(o, TNY_EV_TEXT_DELTA, content, strlen(content));
    }
    const char *reasoning = jget_str(delta, "reasoning_content");
    if (!reasoning) reasoning = jget_str(delta, "reasoning");
    if (reasoning && *reasoning)
        emit_text(o, TNY_EV_THINKING, reasoning, strlen(reasoning));

    yyjson_val *tcs = jget(delta, "tool_calls");
    if (tcs && yyjson_is_arr(tcs)) {
        size_t idx, max;
        yyjson_val *tc;
        yyjson_arr_foreach(tcs, idx, max, tc) {
            int64_t index = jget_int(tc, "index", (int64_t)idx);
            if (index < 0 || index >= MAX_TOOL_CALLS) continue;
            while (o->ncalls <= index) {
                pending_call *pc = &o->calls[o->ncalls++];
                pc->id = NULL;
                pc->name = NULL;
                buf_init(&pc->args);
            }
            pending_call *pc = &o->calls[index];
            const char *id = jget_str(tc, "id");
            if (id && !pc->id) pc->id = xstrdup(id);
            yyjson_val *fn = jget(tc, "function");
            const char *name = jget_str(fn, "name");
            if (name && !pc->name) pc->name = xstrdup(name);
            const char *frag = jget_str(fn, "arguments");
            if (frag) buf_appends(&pc->args, frag);
        }
    }
    yyjson_doc_free(doc);
}

/* ---------- step completion ---------- */

static void log_toolcall(oa_impl *o, const char *name, bool ok) {
    if (o->toolcall_log.len > 1) buf_appends(&o->toolcall_log, ",");
    buf_appendf(&o->toolcall_log, "{\"name\":\"%s\",\"status\":\"%s\"}",
                name, ok ? "success" : "error");
}

static void finish_turn_ok(oa_impl *o) {
    tny_session *s = o->env.session;
    session_add_assistant(s, o->text.len ? o->text.data : "", NULL);
    session_bump_turns(s);
    session_compact(s, false);
    session_save(s);
    session_recovery_clear(s);
    if (o->usage_in || o->usage_out) {
        session_add_usage(s, o->usage_in, o->usage_out);
        session_save(s);
        tny_event ev = {0};
        ev.kind = TNY_EV_USAGE;
        ev.in_tokens = o->usage_in;
        ev.out_tokens = o->usage_out;
        emit(o, &ev);
    }
    emit_turn_end(o, TNY_STOP_DONE);
}

static int step_finished(oa_impl *o) {
    tny_session *s = o->env.session;
    if (o->ncalls == 0) {
        finish_turn_ok(o);
        return 0;
    }

    /* record assistant message with tool_calls */
    buf_t tcj;
    buf_init(&tcj);
    buf_appends(&tcj, "[");
    for (int i = 0; i < o->ncalls; i++) {
        pending_call *pc = &o->calls[i];
        if (i) buf_appends(&tcj, ",");
        buf_appendf(&tcj, "{\"id\":\"%s\",\"type\":\"function\",\"function\":{\"name\":\"%s\",\"arguments\":",
                    pc->id ? pc->id : "call_0", pc->name ? pc->name : "unknown");
        jescape(&tcj, pc->args.data ? pc->args.data : "{}");
        buf_appends(&tcj, "}}");
    }
    buf_appends(&tcj, "]");
    session_add_assistant(s, o->text.len ? o->text.data : NULL, tcj.data);
    buf_free(&tcj);

    /* execute tools serially (writes must not race; reads are cheap) */
    for (int i = 0; i < o->ncalls; i++) {
        pending_call *pc = &o->calls[i];
        const char *name = pc->name ? pc->name : "unknown";
        tny_event ev = {0};
        ev.kind = TNY_EV_TOOL_START;
        ev.tool_name = name;
        ev.tool_id = pc->id;
        ev.tool_detail = pc->args.data;
        emit(o, &ev);

        char *result = tools_execute(&o->env, name, pc->args.data ? pc->args.data : "{}");
        bool ok = !str_starts(result, "error:");
        log_toolcall(o, name, ok);

        tny_event ev2 = {0};
        ev2.kind = TNY_EV_TOOL_END;
        ev2.tool_name = name;
        ev2.tool_id = pc->id;
        ev2.tool_detail = result;
        ev2.tool_ok = ok;
        emit(o, &ev2);

        session_add_tool_result(s, pc->id ? pc->id : "call_0", result);
        free(result);

        if (o->cancelled) break;
    }
    if (o->env.n_pending_images) {
        char ierr[256];
        if (tools_flush_images(&o->env, ierr, sizeof ierr) != 0)
            emit_text(o, TNY_EV_ERROR, ierr, strlen(ierr));
    }
    calls_reset(o);
    session_save(s);

    if (o->env.perm_blocked && !o->env.prompt) {
        emit_turn_end(o, TNY_STOP_DENIED);
        return 0;
    }
    if (o->cancelled) {
        emit_turn_end(o, TNY_STOP_INTERRUPTED);
        return 0;
    }
    o->step++;
    if (o->step >= o->ctx->max_steps) {
        emit_text(o, TNY_EV_ERROR, "step limit reached", 18);
        session_bump_turns(s);
        session_save(s);
        emit_turn_end(o, TNY_STOP_STEP_LIMIT);
        return 0;
    }
    char err[512];
    if (start_post(o, err, sizeof err) != 0) {
        emit_text(o, TNY_EV_ERROR, err, strlen(err));
        emit_turn_end(o, TNY_STOP_ERROR);
        return -1;
    }
    return 0;
}

/* ---------- vtable ---------- */

static int oa_connect(tny_backend *b, char *errbuf, size_t errlen) {
    oa_impl *o = b->impl;
    if (!o->ctx->api_key &&
        !str_starts(o->ctx->base_url, "http://")) {
        snprintf(errbuf, errlen,
                 "no API key: set OPENAI_API_KEY (or --api-key-env NAME; "
                 "local http:// providers may omit it)%s",
                 tny_codex_auth_present()
                     ? ". A codex login exists — `tny --provider codex` uses it"
                     : "");
        return -1;
    }
    return 0;
}

static void oa_disconnect(tny_backend *b) {
    oa_impl *o = b->impl;
    if (o->conn) { http_close(o->conn); o->conn = NULL; }
}

static int oa_create_or_resume(tny_backend *b, const char *ptr, char *e, size_t el) {
    (void)b; (void)ptr; (void)e; (void)el;
    return 0; /* session handling is local */
}

static char *oa_session_pointer(tny_backend *b) {
    (void)b;
    return NULL;
}

static int oa_send(tny_backend *b, const char *prompt, const char **images,
                   tny_event_cb cb, void *ud, char *errbuf, size_t errlen) {
    oa_impl *o = b->impl;
    if (!o->env.session || !o->env.perm) {
        snprintf(errbuf, errlen, "native backend not bound to a session");
        return -1;
    }
    o->cb = cb;
    o->ud = ud;
    o->step = 0;
    o->cancelled = false;
    o->usage_in = o->usage_out = 0;
    o->env.perm_blocked = false;
    buf_clear(&o->toolcall_log);
    buf_appends(&o->toolcall_log, "[");
    calls_reset(o);

    tny_session *s = o->env.session;
    if (!session_title(s)) session_set_title(s, prompt);
    session_set_meta(s, "openai", model_of(o));

    if (images && images[0]) {
        if (session_add_user_images(s, prompt, images, errbuf, errlen) != 0)
            return -1;
    } else {
        session_add_text(s, "user", prompt);
    }
    session_save(s);
    return start_post(o, errbuf, errlen);
}

static void oa_cancel(tny_backend *b) {
    oa_impl *o = b->impl;
    if (o->state == ST_IDLE) return;
    o->cancelled = true;
    if (o->text.len) {
        session_recovery_write(o->env.session, o->text.data);
        session_add_assistant(o->env.session, o->text.data, NULL);
        session_save(o->env.session);
    }
    oa_disconnect(b);
    emit_turn_end(o, TNY_STOP_INTERRUPTED);
}

static void oa_respond_permission(tny_backend *b, const char *id, tny_perm_decision d) {
    (void)b; (void)id; (void)d; /* native approvals flow through the prompt hook */
}

static int oa_pollfds(tny_backend *b, struct pollfd *fds, int max) {
    oa_impl *o = b->impl;
    if (o->state == ST_IDLE || !o->conn || max < 1) return 0;
    fds[0].fd = http_fd(o->conn);
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    return 1;
}

static int oa_dispatch(tny_backend *b, struct pollfd *fds, int n) {
    (void)fds; (void)n;
    oa_impl *o = b->impl;
    if (o->state == ST_IDLE || !o->conn) return 0;

    if (o->state == ST_HEADERS) {
        int status = http_read_response(o->conn, 0);
        if (status == -2) return 0;
        if (status < 0) {
            emit_text(o, TNY_EV_ERROR, "connection lost before response", 31);
            emit_turn_end(o, TNY_STOP_ERROR);
            return -1;
        }
        if (status == 401 || status == 403) {
            emit_text(o, TNY_EV_ERROR, "authentication failed (401/403): check the API key", 51);
            emit_turn_end(o, TNY_STOP_ERROR);
            return -1;
        }
        if (status >= 400) {
            char body[2048];
            buf_t msg;
            buf_init(&msg);
            buf_appendf(&msg, "provider returned HTTP %d", status);
            ssize_t bn = http_body_read(o->conn, body, sizeof body - 1);
            if (bn > 0) {
                body[bn] = 0;
                buf_appendf(&msg, ": %.900s", body);
            }
            emit_text(o, TNY_EV_ERROR, msg.data, msg.len);
            buf_free(&msg);
            emit_turn_end(o, TNY_STOP_ERROR);
            return -1;
        }
        o->state = ST_BODY;
    }

    for (;;) {
        char tmp[16384];
        ssize_t bn = http_body_read(o->conn, tmp, sizeof tmp);
        if (bn == -2) return 0;
        if (bn > 0) {
            sse_feed(&o->sse, tmp, (size_t)bn, on_sse_event, o);
            if (o->cancelled) return 0;
            continue;
        }
        /* 0 = body complete; -1 = transport error mid-stream */
        if (bn < 0 && !o->stream_done) {
            /* keep partial text recoverable */
            if (o->text.len) session_recovery_write(o->env.session, o->text.data);
            emit_text(o, TNY_EV_ERROR, "stream aborted mid-response", 27);
            emit_turn_end(o, TNY_STOP_ERROR);
            return -1;
        }
        return step_finished(o);
    }
}

static int oa_doctor(struct tny_ctx *ctx, char *line, size_t linelen) {
    if (ctx->api_key) {
        snprintf(line, linelen, "openai: key present, base_url %s", ctx->base_url);
        return 0;
    }
    if (str_starts(ctx->base_url, "http://")) {
        snprintf(line, linelen, "openai: local provider %s (no key needed)", ctx->base_url);
        return 0;
    }
    snprintf(line, linelen, "openai: no API key (set OPENAI_API_KEY or run tny setup)");
    return 1;
}

static void oa_destroy(tny_backend *b) {
    oa_impl *o = b->impl;
    oa_disconnect(b);
    calls_reset(o);
    buf_free(&o->text);
    buf_free(&o->toolcall_log);
    sse_parser_free(&o->sse);
    free(o);
    free(b);
}

void tny_backend_openai_bind(tny_backend *b, tny_session *session,
                             perm_engine *perm,
                             tny_perm_decision (*prompt)(const char *, const char *, void *),
                             void *prompt_ud) {
    oa_impl *o = b->impl;
    o->env.session = session;
    o->env.perm = perm;
    o->env.prompt = prompt;
    o->env.prompt_ud = prompt_ud;
}

int tny_backend_openai_steps(tny_backend *b) {
    oa_impl *o = b->impl;
    return o->step + 1;
}

const char *tny_backend_openai_toolcalls_json(tny_backend *b) {
    oa_impl *o = b->impl;
    if (!o->toolcall_log.len) return "[]";
    if (o->toolcall_log.data[o->toolcall_log.len - 1] != ']')
        buf_appends(&o->toolcall_log, "]");
    return o->toolcall_log.data;
}

char *tny_openai_response_format(const char *schema_json, size_t len) {
    yyjson_doc *doc = jparse(schema_json, len);
    if (!doc) return NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc *m = yyjson_mut_doc_new(NULL);
    if (!m) { yyjson_doc_free(doc); return NULL; }
    yyjson_mut_val *copy = yyjson_val_mut_copy(m, root);
    const char *type = jget_str(root, "type");
    yyjson_mut_val *rf;
    if (type && strcmp(type, "json_schema") == 0) {
        rf = copy; /* already a full response_format */
    } else {
        rf = yyjson_mut_obj(m);
        yyjson_mut_obj_add_str(m, rf, "type", "json_schema");
        yyjson_mut_val *js;
        if (jget(root, "schema")) {
            js = copy; /* already a json_schema object ({name, schema, …}) */
            if (!jget(root, "name"))
                yyjson_mut_obj_add_str(m, js, "name", "output");
        } else {
            js = yyjson_mut_obj(m);
            yyjson_mut_obj_add_str(m, js, "name", "output");
            yyjson_mut_obj_add_bool(m, js, "strict", true);
            yyjson_mut_obj_add_val(m, js, "schema", copy);
        }
        yyjson_mut_obj_add_val(m, rf, "json_schema", js);
    }
    yyjson_mut_doc_set_root(m, rf);
    char *out = jwrite(m);
    yyjson_mut_doc_free(m);
    yyjson_doc_free(doc);
    return out;
}

tny_backend *tny_backend_openai_new(struct tny_ctx *ctx) {
    tny_backend *b = calloc(1, sizeof *b);
    oa_impl *o = calloc(1, sizeof *o);
    if (!b || !o) { free(b); free(o); return NULL; }
    o->ctx = ctx;
    o->env.ctx = ctx;
    buf_init(&o->text);
    buf_init(&o->toolcall_log);
    sse_parser_init(&o->sse);
    b->id = TNY_BK_OPENAI;
    b->impl = o;
    b->connect = oa_connect;
    b->disconnect = oa_disconnect;
    b->create_or_resume = oa_create_or_resume;
    b->session_pointer = oa_session_pointer;
    b->send = oa_send;
    b->cancel = oa_cancel;
    b->respond_permission = oa_respond_permission;
    b->pollfds = oa_pollfds;
    b->dispatch = oa_dispatch;
    b->doctor = oa_doctor;
    b->destroy = oa_destroy;
    return b;
}
