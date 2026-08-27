/* openai.c — native OpenAI-compatible backend: Responses API SSE (default)
 * or Chat Completions SSE (wire_api "chat"), plus the tny-owned tool loop
 * (docs/backends/openai-compatible.md, docs/adr/0016). */
#include "backends/openai/openai.h"
#include "core/tools.h"
#include "core/image.h"
#include "core/instructions.h"
#include "core/skills.h"
#include "net/net.h"
#include "util/alloc.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>

#define OPENAI_DEFAULT_MODEL "gpt-4.1-mini"

typedef enum { ST_IDLE, ST_HEADERS, ST_BODY, ST_WAIT_PERMISSION } oa_state;

typedef struct {
    char *id;
    tools_call call;
    tny_perm_decision decision;
    char *original_args;
    char *effective_args;
    char *control_extension;
    char *control_reason;
} oa_pending_perm;

typedef struct {
    tny_ctx *ctx;
    tools_env env;
    http_conn *conn;
    sse_parser sse;
    oa_state state;

    tny_backend_event_cb cb;
    void *ud;
    tny_openai_control_cb control;
    void *control_ud;

    buf_t text;             /* assistant text this step */
    oa_callset calls;       /* streamed tool_calls this step (toolcalls.c) */
    int  step;
    bool cancelled;
    bool conn_reused;       /* this POST rode a kept-alive connection */
    bool wire_chat;         /* this POST rides the legacy chat wire */
    bool stream_done;       /* saw [DONE] / response.completed */
    bool stream_failed;     /* responses wire signalled a terminal error */
    tny_stop_reason final_stop; /* provider terminal reason for this step */
    char finish_reason[32];
    int64_t usage_in, usage_out;
    uint64_t provider_request_sequence;
    int provider_attempt;

    buf_t toolcall_log;     /* JSON array text for ask --json */
    int tool_index;         /* next call in the recorded assistant batch */
    int tool_batch_failed;
    bool tool_batch_active;
    oa_pending_perm pending_perm;
    char *steer;            /* user text parked by steer(): appended as a
                             * user message before the next POST (adr/0011) */
    char errbuf[512];
} oa_impl;

static void pending_perm_clear(oa_impl *o) {
    free(o->pending_perm.id);
    free(o->pending_perm.original_args);
    free(o->pending_perm.effective_args);
    free(o->pending_perm.control_extension);
    free(o->pending_perm.control_reason);
    tools_call_free(&o->pending_perm.call);
    memset(&o->pending_perm, 0, sizeof o->pending_perm);
}

static void permission_block(oa_impl *o) { o->env.perm_blocked = true; }

static void control_response_free(tny_openai_control_response *response) {
    if (!response) return;
    free(response->arguments_json);
    free(response->result);
    free(response->extension);
    free(response->reason);
    memset(response, 0, sizeof *response);
}

static tny_openai_control_response control_call(
    oa_impl *o, const tny_openai_control_request *request) {
    tny_openai_control_response response = {0};
    if (o->control) o->control(request, &response, o->control_ud);
    return response;
}

/* Move parked steer text into the transcript as a user message. */
static bool take_steer(oa_impl *o) {
    if (!o->steer) return false;
    session_add_text(o->env.session, "user", o->steer);
    free(o->steer);
    o->steer = NULL;
    return true;
}

/* ---------- helpers ---------- */

static void emit(oa_impl *o, const tny_backend_event *ev) {
    if (o->cb) o->cb(ev, o->ud);
}

static void emit_text(oa_impl *o, tny_event_kind k, const char *t, size_t n) {
    tny_backend_event ev = {0};
    ev.kind = k;
    ev.text = t;
    ev.text_len = n;
    emit(o, &ev);
}

static void emit_error(oa_impl *o, tny_event_error_kind code,
                       const char *text, size_t len) {
    tny_backend_event ev = {0};
    ev.kind = TNY_EV_ERROR;
    ev.error_code = code;
    ev.text = text;
    ev.text_len = len;
    emit(o, &ev);
}

static void emit_turn_end(oa_impl *o, tny_stop_reason stop) {
    o->state = ST_IDLE;
    pending_perm_clear(o);
    o->tool_batch_active = false;
    if (o->steer) {
        /* the turn is ending with the steered text still parked (interrupt,
         * error, step limit): hand it back so it is never silently lost
         * (docs/adr/0013) */
        emit_text(o, TNY_EV_STEER_REJECTED, o->steer, strlen(o->steer));
        free(o->steer);
        o->steer = NULL;
    }
    tny_backend_event ev = {0};
    ev.kind = TNY_EV_TURN_END;
    ev.stop = stop;
    emit(o, &ev);
}

static const char *model_of(oa_impl *o) {
    return o->ctx->model ? o->ctx->model : OPENAI_DEFAULT_MODEL;
}

/* The shared system preamble: workspace, AGENTS.md chain, skill catalog. */
static void build_system_prompt(oa_impl *o, buf_t *sys) {
    buf_appends(sys, "You are tny, a fast coding agent running in a terminal.\n");
    if (o->ctx->ssh_host) {
        /* --ssh (docs/adr/0022): the tools act on another machine; the
         * local workspace only supplies config. Say so, or the model
         * "corrects" pwd against the local path it was told about. */
        buf_appendf(sys,
            "You are working in a REMOTE environment: every workspace tool "
            "(files, grep, terminal) executes over SSH on %s. The local "
            "machine running tny is not your workspace.\n"
            "Current working directory (remote): %s\n"
            "Relative paths resolve against it; the terminal starts there.\n",
            o->ctx->ssh_host, o->ctx->ssh_cwd);
    } else {
        buf_appendf(sys, "Primary workspace: %s\n", o->ctx->cwd);
        for (int i = 0; i < o->ctx->n_extra_dirs; i++)
            buf_appendf(sys, "Additional workspace directory: %s\n", o->ctx->extra_dirs[i]);
    }
    buf_appends(sys,
        "Use the provided tools to inspect and change the workspace. Prefer "
        "small, verifiable steps. When you are done, answer in Markdown.\n");
    instructions_collect(o->ctx, sys);
    /* skill catalog: names only, lazy bodies */
    if (!o->ctx->library_mode) {
        int nsk = 0;
        skill_meta *sk = skills_discover(o->ctx, &nsk);
        if (nsk > 0) {
            buf_appends(sys, "\nAvailable skills (load with the `skill` tool):\n");
            for (int i = 0; i < nsk; i++)
                buf_appendf(sys, "- %s: %.140s\n", sk[i].name, sk[i].description);
        }
        skills_free(sk, nsk);
    }
}

/* Build the legacy Chat Completions request body from the session view. */
static char *build_request_chat(oa_impl *o) {
    tny_session_state *s = o->env.session;
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"model\":");
    jescape(&b, model_of(o));
    /* TNY_CAP_FAST: OpenAI's paid fast tier ("priority" pre-rename; both
     * spellings select it — send "priority", which older models and
     * compatible routers also accept). Omitted otherwise: "default" is
     * what the API applies anyway, and strict providers reject unknown
     * request members. */
    if (tny_tier_is_fast(o->ctx->service_tier))
        buf_appends(&b, ",\"service_tier\":\"priority\"");
    buf_appends(&b, ",\"stream\":true,\"messages\":[");

    buf_t sys;
    buf_init(&sys);
    build_system_prompt(o, &sys);
    if (buf_oom(&sys)) { buf_free(&sys); buf_free(&b); return NULL; }
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
        char *mj = jwrite_mut_val(m);
        if (mj) {
            buf_appends(&b, ",");
            buf_appends(&b, mj);
            free(mj);
        }
    }
    buf_appends(&b, "]");

    char *schema = tools_schema_json(&o->env);
    if (!schema) { buf_free(&b); return NULL; }
    buf_appendf(&b, ",\"tools\":%s,\"tool_choice\":\"auto\"", schema);
    free(schema);
    if (o->ctx->output_schema)
        buf_appendf(&b, ",\"response_format\":%s", o->ctx->output_schema);
    if (o->ctx->max_tokens_field)
        buf_appendf(&b, ",\"%s\":8192", o->ctx->max_tokens_field);
    /* read per request, so /effort applies from the next turn */
    if (o->ctx->reasoning_effort && *o->ctx->reasoning_effort) {
        buf_appends(&b, ",\"reasoning_effort\":");
        jescape(&b, tny_effort_wire(TNY_BK_OPENAI, o->ctx->reasoning_effort));
    }
    buf_appends(&b, "}");
    return buf_detach(&b);
}

/* Build the Responses API request body (docs/adr/0016): the stored chat
 * shape is translated onto `input` items, tools ride flat, structured
 * output rides `text.format`, and reasoning effort rides
 * `reasoning.effort`. store:false — tny owns session state, never the
 * provider. */
static char *build_request_rsp(oa_impl *o) {
    tny_session_state *s = o->env.session;
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"model\":");
    jescape(&b, model_of(o));
    if (tny_tier_is_fast(o->ctx->service_tier))
        buf_appends(&b, ",\"service_tier\":\"priority\"");
    buf_appends(&b, ",\"stream\":true,\"store\":false");

    buf_t sys;
    buf_init(&sys);
    build_system_prompt(o, &sys);
    if (buf_oom(&sys)) { buf_free(&sys); buf_free(&b); return NULL; }
    buf_appends(&b, ",\"instructions\":");
    jescape(&b, sys.data);
    buf_free(&sys);

    const char *summary = NULL;
    int boundary = session_compact_boundary(s, &summary);
    char *input = tny_openai_responses_input(session_messages(s), boundary, summary);
    buf_appendf(&b, ",\"input\":%s", input ? input : "[]");
    free(input);

    char *schema = tools_schema_json(&o->env);
    if (!schema) { buf_free(&b); return NULL; }
    char *flat = tny_openai_responses_tools(schema);
    buf_appendf(&b, ",\"tools\":%s,\"tool_choice\":\"auto\"", flat ? flat : "[]");
    free(flat);
    free(schema);

    if (o->ctx->output_schema) {
        char *fmt = tny_openai_responses_text_format(o->ctx->output_schema);
        if (fmt) {
            buf_appendf(&b, ",\"text\":{\"format\":%s}", fmt);
            free(fmt);
        }
    }
    /* max_tokens_field set means the user wants a completion cap; the
     * Responses wire spells it max_output_tokens whatever the chat quirk */
    if (o->ctx->max_tokens_field)
        buf_appends(&b, ",\"max_output_tokens\":8192");
    if (o->ctx->reasoning_effort && *o->ctx->reasoning_effort) {
        buf_appends(&b, ",\"reasoning\":{\"effort\":");
        jescape(&b, tny_effort_wire(TNY_BK_OPENAI, o->ctx->reasoning_effort));
        buf_appends(&b, "}");
    }
    buf_appends(&b, "}");
    return buf_detach(&b);
}

static tny_openai_control_response provider_control(
    oa_impl *o, tny_openai_control_kind kind, int status) {
    char request_id[192];
    snprintf(request_id, sizeof request_id, "%s:%llu:request:%llu",
             o->env.session ? o->env.session->id : "session",
             (unsigned long long)(o->env.session
                 ? o->env.session->extension_agent_sequence : 0),
             (unsigned long long)o->provider_request_sequence);
    tny_openai_control_request provider = {0};
    provider.kind = kind;
    provider.method = "POST";
    provider.endpoint = o->wire_chat ? "/chat/completions" : "/responses";
    provider.status = status;
    provider.stream = true;
    provider.connection_reused = o->conn_reused;
    provider.wire_api = o->wire_chat ? "chat" : "responses";
    provider.step = o->step;
    provider.logical_request_id = request_id;
    provider.attempt = o->provider_attempt;
    return control_call(o, &provider);
}

static int start_post_mode(oa_impl *o, char *errbuf, size_t errlen,
                           bool retry) {
    char err[256] = {0};
    if (retry) o->provider_attempt++;
    else {
        o->provider_request_sequence++;
        o->provider_attempt = 1;
    }
    o->conn_reused = o->conn != NULL;
    if (!o->conn) {
        o->conn = http_open(o->ctx->base_url, err, sizeof err);
        if (!o->conn) {
            /* Preserve actionable errors generated by our TLS shim without
             * exposing a configured URL or provider-supplied response text. */
            if (str_starts(err, "TLS ") || str_starts(err, "https not built"))
                snprintf(errbuf, errlen, "%s", err);
            else
                snprintf(errbuf, errlen, "could not connect to provider");
            return -1;
        }
    }
    /* the wire is read per POST so /provider and settings edits apply on
     * the next request, and every event in one stream parses consistently */
    o->wire_chat = tny_wire_is_chat(o->ctx->wire_api);
    char *body = o->wire_chat ? build_request_chat(o) : build_request_rsp(o);
    buf_t auth;
    buf_init(&auth);
    buf_appendf(&auth, "%s: %s%s", o->ctx->auth_header_name,
                o->ctx->auth_header_prefix, o->ctx->api_key ? o->ctx->api_key : "");
    const char *hdrs[12];
    int hn = 0;
    hdrs[hn++] = "Content-Type: application/json";
    hdrs[hn++] = "Accept: text/event-stream";
    if (o->ctx->api_key) hdrs[hn++] = auth.data;
    /* builtin-profile headers (claude oauth beta, grok proxy auth/model
     * routing — docs/adr/0019) */
    for (char **e = o->ctx->extra_headers; e && *e && hn < 11; e++)
        hdrs[hn++] = *e;
    hdrs[hn] = NULL;
    buf_t path;
    buf_init(&path);
    buf_appendf(&path, "%s%s", http_prefix(o->conn),
                o->wire_chat ? "/chat/completions" : "/responses");
    if (!body || buf_oom(&auth) || buf_oom(&path) ||
        tny_alloc_scope_failed()) {
        snprintf(errbuf, errlen, "out of memory");
        buf_free(&path);
        if (auth.data) secure_zero(auth.data, auth.len);
        buf_free(&auth);
        free(body);
        return -1;
    }
    tny_openai_control_response control = provider_control(
        o, TNY_OPENAI_CONTROL_PROVIDER_REQUEST, 0);
    if (control.stop) {
        o->cancelled = true;
        control_response_free(&control);
        buf_free(&path);
        if (auth.data) secure_zero(auth.data, auth.len);
        buf_free(&auth);
        free(body);
        emit_turn_end(o, TNY_STOP_INTERRUPTED);
        return 0;
    }
    control_response_free(&control);
    int rc = http_request(o->conn, "POST", path.data, hdrs, body, strlen(body));
    if (rc != 0) {
        /* stale keep-alive caught at write time: reopen once */
        control = provider_control(o, TNY_OPENAI_CONTROL_PROVIDER_RESPONSE, 0);
        if (control.stop) o->cancelled = true;
        control_response_free(&control);
        http_close(o->conn);
        o->conn = http_open(o->ctx->base_url, err, sizeof err);
        o->conn_reused = false;
        if (o->conn) {
            o->provider_attempt++;
            control = provider_control(o, TNY_OPENAI_CONTROL_PROVIDER_REQUEST, 0);
            if (control.stop) {
                o->cancelled = true;
                control_response_free(&control);
                rc = -1;
            } else {
                control_response_free(&control);
                rc = http_request(o->conn, "POST", path.data, hdrs, body,
                                  strlen(body));
                if (rc != 0) {
                    control = provider_control(
                        o, TNY_OPENAI_CONTROL_PROVIDER_RESPONSE, 0);
                    if (control.stop) o->cancelled = true;
                    control_response_free(&control);
                }
            }
        }
    }
    buf_free(&path);
    if (auth.data) secure_zero(auth.data, auth.len);
    buf_free(&auth);
    free(body);
    if (rc != 0) {
        snprintf(errbuf, errlen, "provider request failed");
        return -1;
    }
    o->state = ST_HEADERS;
    o->stream_done = false;
    o->stream_failed = false;
    o->final_stop = TNY_STOP_DONE;
    o->finish_reason[0] = 0;
    buf_clear(&o->text);
    sse_parser_free(&o->sse);
    sse_parser_init(&o->sse);
    return 0;
}

static int start_post(oa_impl *o, char *errbuf, size_t errlen) {
    return start_post_mode(o, errbuf, errlen, false);
}

/* ---------- SSE event handling ---------- */

static void on_sse_event_chat(const char *data, size_t len, void *ud) {
    oa_impl *o = ud;
    if ((len == 6 && memcmp(data, "[DONE]", 6) == 0) ||
        (len == 4 && memcmp(data, "DONE", 4) == 0)) {
        o->stream_done = true;
        return;
    }
    yyjson_doc *doc = jparse(data, len);
    if (!doc) return; /* never block the loop on a parse error */
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *root_error = jget(root, "error");
    if (root_error) {
        const char *message = jget_str(root_error, "message");
        buf_t error;
        buf_init(&error);
        (void)message;
        buf_appends(&error, "provider stream reported an error");
        emit_error(o, TNY_EVENT_ERROR_PROTOCOL, error.data, error.len);
        buf_free(&error);
        o->stream_done = true;
        o->stream_failed = true;
        yyjson_doc_free(doc);
        return;
    }
    yyjson_val *usage = jget(root, "usage");
    if (usage) {
        o->usage_in = jget_int(usage, "prompt_tokens", o->usage_in);
        o->usage_out = jget_int(usage, "completion_tokens", o->usage_out);
    }
    yyjson_val *choice = yyjson_arr_get_first(jget(root, "choices"));
    if (!choice) { yyjson_doc_free(doc); return; }
    const char *fr = jget_str(choice, "finish_reason");
    if (fr) {
        snprintf(o->finish_reason, sizeof o->finish_reason, "%s", fr);
        if (strcmp(fr, "length") == 0) o->final_stop = TNY_STOP_STEP_LIMIT;
        else if (strcmp(fr, "content_filter") == 0)
            o->final_stop = TNY_STOP_DENIED;
        else if (strcmp(fr, "error") == 0) {
            emit_error(o, TNY_EVENT_ERROR_PROTOCOL,
                       "provider ended the stream with an error", 39);
            o->stream_failed = true;
        }
    }
    yyjson_val *delta = jget(choice, "delta");
    if (!delta) delta = jget(choice, "message"); /* non-stream fallback */
    size_t content_len = 0;
    const char *content = jget_strn(delta, "content", &content_len);
    if (content && content_len) {
        buf_append(&o->text, content, content_len);
        emit_text(o, TNY_EV_TEXT_DELTA, content, content_len);
    }
    size_t reasoning_len = 0;
    const char *reasoning = jget_strn(delta, "reasoning_content", &reasoning_len);
    if (!reasoning) reasoning = jget_strn(delta, "reasoning", &reasoning_len);
    if (reasoning && reasoning_len)
        emit_text(o, TNY_EV_THINKING, reasoning, reasoning_len);
    else {
        yyjson_val *details = jget(delta, "reasoning_details");
        size_t idx, max;
        yyjson_val *detail;
        if (details && yyjson_is_arr(details)) {
            yyjson_arr_foreach(details, idx, max, detail) {
                size_t text_len = 0;
                const char *text = jget_strn(detail, "text", &text_len);
                if (!text) text = jget_strn(detail, "summary", &text_len);
                if (text && text_len)
                    emit_text(o, TNY_EV_THINKING, text, text_len);
            }
        }
    }

    oa_calls_feed(&o->calls, jget(delta, "tool_calls"));
    yyjson_doc_free(doc);
}

/* Responses wire: pending call for one output_index, or NULL. The
 * item's output_index lives in oa_call.wire_index (the chat wire's
 * "index" slot — both are the provider's per-call ordinal). */
static oa_call *rsp_call_by_index(oa_impl *o, int64_t oindex) {
    for (int i = 0; i < o->calls.n; i++)
        if (o->calls.calls[i].wire_index == oindex) return &o->calls.calls[i];
    return NULL;
}

/* Typed Responses API events (docs/adr/0016). The SSE parser drops the
 * `event:` line; every payload repeats the type in its "type" member, so
 * dispatch happens on the data alone. */
static void on_sse_event_rsp(const char *data, size_t len, void *ud) {
    oa_impl *o = ud;
    yyjson_doc *doc = jparse(data, len);
    /* parse errors never block the loop. A stray "[DONE]" from a
     * chat-flavored gateway lands here too: it is not JSON, and the
     * Responses stream ends on response.completed, not the sentinel. */
    if (!doc) return;
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *type = jget_str(root, "type");
    if (!type) { yyjson_doc_free(doc); return; }

    if (strcmp(type, "response.output_text.delta") == 0) {
        size_t delta_len = 0;
        const char *d = jget_strn(root, "delta", &delta_len);
        if (d && delta_len) {
            buf_append(&o->text, d, delta_len);
            emit_text(o, TNY_EV_TEXT_DELTA, d, delta_len);
        }
    } else if (strcmp(type, "response.reasoning_summary_text.delta") == 0 ||
               strcmp(type, "response.reasoning_text.delta") == 0) {
        size_t delta_len = 0;
        const char *d = jget_strn(root, "delta", &delta_len);
        if (d && delta_len) emit_text(o, TNY_EV_THINKING, d, delta_len);
    } else if (strcmp(type, "response.output_item.added") == 0 ||
               strcmp(type, "response.output_item.done") == 0) {
        yyjson_val *item = jget(root, "item");
        const char *itype = jget_str(item, "type");
        if (itype && strcmp(itype, "function_call") == 0) {
            int64_t oindex = jget_int(root, "output_index", o->calls.n);
            oa_call *pc = rsp_call_by_index(o, oindex);
            if (!pc && oindex >= 0 && o->calls.n < OA_MAX_TOOL_CALLS) {
                pc = &o->calls.calls[o->calls.n++];
                pc->id = NULL;
                pc->name = NULL;
                buf_init(&pc->args);
                pc->wire_index = (int)oindex;
            }
            if (pc) {
                const char *id = jget_str(item, "call_id");
                if (id && !pc->id) pc->id = xstrdup(id);
                const char *name = jget_str(item, "name");
                if (name && !pc->name) pc->name = xstrdup(name);
                /* item.done carries the complete argument string — it is
                 * authoritative over deltas assembled along the way */
                const char *args = jget_str(item, "arguments");
                if (args && *args) {
                    buf_clear(&pc->args);
                    buf_appends(&pc->args, args);
                }
            }
        }
    } else if (strcmp(type, "response.function_call_arguments.delta") == 0) {
        oa_call *pc = rsp_call_by_index(o, jget_int(root, "output_index", -1));
        const char *d = jget_str(root, "delta");
        if (pc && d) buf_appends(&pc->args, d);
    } else if (strcmp(type, "response.completed") == 0) {
        yyjson_val *usage = jget(jget(root, "response"), "usage");
        if (usage) {
            o->usage_in = jget_int(usage, "input_tokens", o->usage_in);
            o->usage_out = jget_int(usage, "output_tokens", o->usage_out);
        }
        o->stream_done = true;
    } else if (strcmp(type, "response.incomplete") == 0) {
        /* token/limit cutoff: keep the partial text, end the step cleanly
         * (the chat wire treats finish_reason "length" the same way) */
        yyjson_val *response = jget(root, "response");
        const char *reason = jget_str(jget(response, "incomplete_details"),
                                      "reason");
        o->final_stop = reason && strstr(reason, "content_filter")
            ? TNY_STOP_DENIED : TNY_STOP_STEP_LIMIT;
        o->stream_done = true;
    } else if (strcmp(type, "response.failed") == 0 ||
               strcmp(type, "error") == 0) {
        const char *message = "provider stream reported an error";
        emit_error(o, TNY_EVENT_ERROR_PROTOCOL, message, strlen(message));
        o->stream_done = true;
        o->stream_failed = true;
    }
    yyjson_doc_free(doc);
}

static void on_sse_event(const char *data, size_t len, void *ud) {
    oa_impl *o = ud;
    if (o->wire_chat) on_sse_event_chat(data, len, ud);
    else on_sse_event_rsp(data, len, ud);
}

/* ---------- step completion ---------- */

static void log_toolcall(oa_impl *o, const char *name, bool original_ok,
                         bool effective_ok, bool transformed) {
    if (o->toolcall_log.len > 1) buf_appends(&o->toolcall_log, ",");
    buf_appends(&o->toolcall_log, "{\"name\":");
    jescape(&o->toolcall_log, name);
    if (!transformed && original_ok == effective_ok) {
        buf_appendf(&o->toolcall_log, ",\"status\":\"%s\"}",
                    effective_ok ? "success" : "error");
        return;
    }
    buf_appendf(&o->toolcall_log,
                ",\"status\":\"%s\",\"original_status\":\"%s\","
                "\"result_transformed\":%s}",
                effective_ok ? "success" : "error",
                original_ok ? "success" : "error",
                transformed ? "true" : "false");
}

static void finish_turn_ok(oa_impl *o) {
    tny_session_state *s = o->env.session;
    session_add_assistant(s, o->text.len ? o->text.data : "", NULL);
    session_bump_turns(s);
    if (session_save(s) != 0) {
        const char *message = "could not persist completed turn";
        emit_error(o, TNY_EVENT_ERROR_IO,
                   message, strlen(message));
        emit_turn_end(o, TNY_STOP_ERROR);
        return;
    }
    session_recovery_clear(s);
    if (o->usage_in || o->usage_out) {
        session_add_usage(s, o->usage_in, o->usage_out);
        session_save(s);
        tny_backend_event ev = {0};
        ev.kind = TNY_EV_USAGE;
        ev.in_tokens = o->usage_in;
        ev.out_tokens = o->usage_out;
        emit(o, &ev);
    }
    emit_turn_end(o, o->final_stop);
}

static void emit_tool_end(oa_impl *o, const char *cid, const char *name,
                          const char *result, bool ok) {
    tny_backend_event ev = {0};
    ev.kind = TNY_EV_TOOL_END;
    ev.tool_name = name;
    ev.tool_id = cid;
    ev.tool_detail = result;
    ev.tool_ok = ok;
    emit(o, &ev);
}

static void subagent_control(oa_impl *o, tny_openai_control_kind kind,
                             const char *cid, const tools_call *call,
                             const char *result, bool ok) {
    if (!o->control || !call || strcmp(call->name, "subagent") != 0) return;
    const char *action = jget_str(call->args, "action");
    if (!action || (strcmp(action, "create") != 0 &&
                    strcmp(action, "message") != 0)) return;
    const char *requested_id = jget_str(call->args, "id");
    tny_openai_control_request request = {0};
    request.kind = kind;
    request.subagent_id = requested_id && *requested_id ? requested_id : cid;
    request.subagent_action = action;
    request.subagent_outcome = kind == TNY_OPENAI_CONTROL_SUBAGENT_END
        ? (ok ? "done" : "error") : NULL;
    request.subagent_ok = ok;
    request.result = result;
    tny_openai_control_response response = control_call(o, &request);
    if (response.stop) o->cancelled = true;
    control_response_free(&response);
}

static void complete_tool(oa_impl *o, const char *cid, const char *name,
                          const char *original_args, const char *effective_args,
                          const char *control_extension,
                          const char *control_reason, char *original_result) {
    if (!original_result) return;
    bool original_ok = !str_starts(original_result, "error:");
    emit_tool_end(o, cid, name, original_result, original_ok);

    tny_openai_control_request request = {0};
    request.kind = TNY_OPENAI_CONTROL_POST_TOOL;
    request.tool_id = cid;
    request.tool_name = name;
    request.arguments_json = effective_args;
    request.original_arguments_json = original_args;
    request.result = original_result;
    request.original_ok = original_ok;
    request.control_extension = control_extension;
    request.control_reason = control_reason;
    tny_openai_control_response response = control_call(o, &request);
    const char *effective_result = response.result_replaced
        ? response.result : original_result;
    bool effective_ok = response.result_replaced
        ? !response.result_is_error : original_ok;
    bool transformed = response.result_replaced ||
        strcmp(original_args ? original_args : "{}",
               effective_args ? effective_args : "{}") != 0;
    log_toolcall(o, name, original_ok, effective_ok, transformed);
    if (!effective_ok) o->tool_batch_failed++;
    session_add_tool_result(o->env.session, cid, effective_result);
    control_response_free(&response);
}

static void execute_call(oa_impl *o, const char *cid, const char *original_args,
                         const char *effective_args, const char *control_extension,
                         const char *control_reason, tools_call *call) {
    tny_backend_event start = {0};
    start.kind = TNY_EV_TOOL_START;
    start.tool_name = call->name;
    start.tool_id = cid;
    start.tool_detail = effective_args;
    emit(o, &start);
    subagent_control(o, TNY_OPENAI_CONTROL_SUBAGENT_START, cid, call, NULL, false);
    char *result = o->cancelled
        ? tool_err("interrupted before %s ran", call->name)
        : tools_call_execute(&o->env, call);
    if (!result) return;
    bool ok = !str_starts(result, "error:");
    subagent_control(o, TNY_OPENAI_CONTROL_SUBAGENT_END, cid, call, result, ok);
    complete_tool(o, cid, call->name, original_args, effective_args,
                  control_extension, control_reason, result);
    free(result);
}

static void finish_cancelled_call(oa_impl *o, const char *cid, const char *name,
                                  const char *args) {
    char *result = tool_err("interrupted before %s ran", name);
    complete_tool(o, cid, name, args, args, NULL, "cancelled", result);
    free(result);
}

static bool tool_batch_control(oa_impl *o) {
    if (!o->control) return false;
    buf_t ids;
    buf_init(&ids);
    buf_appends(&ids, "[");
    char idbuf[16];
    for (int i = 0; i < o->calls.n; i++) {
        if (i) buf_appends(&ids, ",");
        jescape(&ids, oa_call_id(&o->calls.calls[i], i, idbuf, sizeof idbuf));
    }
    buf_appends(&ids, "]");
    tny_openai_control_request request = {0};
    request.kind = TNY_OPENAI_CONTROL_TOOL_BATCH;
    request.tool_ids_json = ids.data;
    request.failed_tools = o->tool_batch_failed;
    tny_openai_control_response response = control_call(o, &request);
    bool stop = response.stop;
    control_response_free(&response);
    buf_free(&ids);
    return stop;
}

static int finish_tool_batch(oa_impl *o) {
    tny_session_state *s = o->env.session;
    bool batch_stop = tool_batch_control(o);
    if (o->env.n_pending_images) {
        char ierr[256];
        if (tools_flush_images(&o->env, ierr, sizeof ierr) != 0)
            emit_error(o, TNY_EVENT_ERROR_INTERNAL, ierr, strlen(ierr));
    }
    pending_perm_clear(o);
    o->tool_batch_active = false;
    o->tool_index = 0;
    o->tool_batch_failed = 0;
    oa_calls_reset(&o->calls);
    if (session_save(s) != 0) {
        const char *message = "could not persist completed tool batch";
        emit_error(o, TNY_EVENT_ERROR_IO, message, strlen(message));
        emit_turn_end(o, TNY_STOP_ERROR);
        return -1;
    }

    if (batch_stop) {
        emit_turn_end(o, TNY_STOP_INTERRUPTED);
        return 0;
    }

    if (o->cancelled) {
        emit_turn_end(o, TNY_STOP_INTERRUPTED);
        return 0;
    }
    if (o->env.perm_blocked) {
        emit_turn_end(o, TNY_STOP_DENIED);
        return 0;
    }
    /* checked before the increment so o->step + 1 stays the number of model
     * calls actually made — a capped turn never POSTs again */
    if (o->ctx->max_steps > 0 && o->step + 1 >= o->ctx->max_steps) {
        emit_error(o, TNY_EVENT_ERROR_INTERNAL, "step limit reached", 18);
        session_bump_turns(s);
        session_save(s);
        emit_turn_end(o, TNY_STOP_STEP_LIMIT);
        return 0;
    }
    o->step++;
    if (take_steer(o)) session_save(s); /* after tool results, before next POST */
    char err[512];
    if (start_post(o, err, sizeof err) != 0) {
        emit_error(o, TNY_EVENT_ERROR_IO, err, strlen(err));
        emit_turn_end(o, TNY_STOP_ERROR);
        return -1;
    }
    return 0;
}

static int run_tools(oa_impl *o) {
    char idbuf[16];
    while (o->tool_index < o->calls.n) {
        if (tny_alloc_scope_failed()) return -1;
        oa_call *pc = &o->calls.calls[o->tool_index];
        const char *cid = oa_call_id(pc, o->tool_index, idbuf, sizeof idbuf);
        const char *name = pc->name ? pc->name : "unknown";
        const char *args = pc->args.data ? pc->args.data : "{}";

        if (o->cancelled) {
            finish_cancelled_call(o, cid, name, args);
            o->tool_index++;
            continue;
        }

        if (!o->pending_perm.id) {
            tny_openai_control_request pre = {0};
            pre.kind = TNY_OPENAI_CONTROL_PRE_TOOL;
            pre.tool_id = cid;
            pre.tool_name = name;
            pre.arguments_json = args;
            pre.original_arguments_json = args;
            tny_openai_control_response response = control_call(o, &pre);
            const char *effective = response.arguments_json
                ? response.arguments_json : args;
            char *effective_args = xstrdup(effective);
            char *control_extension = response.extension
                ? xstrdup(response.extension) : NULL;
            char *control_reason = response.reason
                ? xstrdup(response.reason) : NULL;
            if (!effective_args || (response.extension && !control_extension) ||
                (response.reason && !control_reason)) {
                free(effective_args);
                free(control_extension);
                free(control_reason);
                control_response_free(&response);
                return -1;
            }
            if (response.stop || response.deny) {
                if (response.stop) o->cancelled = true;
                char *result = response.stop
                    ? tool_err("interrupted before %s ran", name)
                    : tool_err("extension denied %s: %s", name,
                               response.reason ? response.reason : "denied");
                complete_tool(o, cid, name, args, effective_args,
                              control_extension, control_reason, result);
                free(result);
                free(effective_args);
                free(control_extension);
                free(control_reason);
                control_response_free(&response);
                o->tool_index++;
                continue;
            }
            control_response_free(&response);

            tools_call call;
            if (tools_call_prepare(&o->env, name, effective_args, &call) != 0) {
                char *result = call.error ? xstrdup(call.error)
                                          : tool_err("cannot prepare tool call %s", name);
                complete_tool(o, cid, name, args, effective_args,
                              control_extension, control_reason, result);
                free(result);
                tools_call_free(&call);
                free(effective_args);
                free(control_extension);
                free(control_reason);
                o->tool_index++;
                continue;
            }
            /* Only a schema-valid rewrite becomes provider transcript truth.
             * The immutable provider proposal remains in extension_audit. */
            session_replace_tool_arguments(o->env.session, cid, effective_args);
            if (session_save(o->env.session) != 0) {
                char *result = tool_err("could not persist admitted tool call");
                complete_tool(o, cid, call.name, args, effective_args,
                              control_extension, control_reason, result);
                free(result);
                tools_call_free(&call);
                free(effective_args);
                free(control_extension);
                free(control_reason);
                emit_error(o, TNY_EVENT_ERROR_IO,
                           "could not persist admitted tool call", 36);
                emit_turn_end(o, TNY_STOP_ERROR);
                return -1;
            }
            if (call.verdict == PERM_DENY) {
                char *result = tool_err("permission denied for %s", call.name);
                permission_block(o);
                complete_tool(o, cid, call.name, args, effective_args,
                              control_extension, "permission rule denied", result);
                free(result);
                tools_call_free(&call);
                free(effective_args);
                free(control_extension);
                free(control_reason);
                o->tool_index++;
                continue;
            }
            if (call.verdict == PERM_ALLOW) {
                execute_call(o, cid, args, effective_args, control_extension,
                             control_reason, &call);
                tools_call_free(&call);
                free(effective_args);
                free(control_extension);
                free(control_reason);
                o->tool_index++;
                continue;
            }

            tny_openai_control_request permission = {0};
            permission.kind = TNY_OPENAI_CONTROL_PERMISSION;
            permission.tool_id = cid;
            permission.tool_name = call.name;
            permission.arguments_json = effective_args;
            permission.original_arguments_json = args;
            permission.permission_summary = call.summary;
            permission.permission_options = TNY_PERM_ALLOW_ONCE |
                                            TNY_PERM_ALLOW_ALWAYS |
                                            TNY_PERM_DENY;
            response = control_call(o, &permission);
            if (response.extension) {
                free(control_extension);
                control_extension = xstrdup(response.extension);
            }
            if (response.reason) {
                free(control_reason);
                control_reason = xstrdup(response.reason);
            }
            if (response.stop ||
                response.permission == TNY_OPENAI_PERMISSION_DENY) {
                if (!response.stop) permission_block(o);
                char *result = response.stop
                    ? tool_err("interrupted before %s ran", call.name)
                    : tool_err("permission denied for %s", call.name);
                complete_tool(o, cid, call.name, args, effective_args,
                              control_extension, control_reason, result);
                free(result);
                control_response_free(&response);
                tools_call_free(&call);
                free(effective_args);
                free(control_extension);
                free(control_reason);
                o->tool_index++;
                continue;
            }
            if (response.permission == TNY_OPENAI_PERMISSION_ALLOW_ONCE) {
                control_response_free(&response);
                execute_call(o, cid, args, effective_args, control_extension,
                             control_reason, &call);
                tools_call_free(&call);
                free(effective_args);
                free(control_extension);
                free(control_reason);
                o->tool_index++;
                continue;
            }
            control_response_free(&response);

            if (o->env.prompt) {
                tny_perm_decision decision = o->env.prompt(call.name, call.summary,
                                                           o->env.prompt_ud);
                if (decision == TNY_PERM_DECISION_ALLOW_ALWAYS)
                    tools_call_grant(&o->env, &call);
                if (decision == TNY_PERM_DECISION_DENY) {
                    permission_block(o);
                    char *result = tool_err("permission denied for %s", call.name);
                    complete_tool(o, cid, call.name, args, effective_args,
                                  control_extension, "user denied", result);
                    free(result);
                } else {
                    execute_call(o, cid, args, effective_args, control_extension,
                                 control_reason, &call);
                }
                tools_call_free(&call);
                free(effective_args);
                free(control_extension);
                free(control_reason);
                o->tool_index++;
                continue;
            }

            o->pending_perm.id = xstrdup(cid);
            o->pending_perm.original_args = xstrdup(args);
            if (!o->pending_perm.id || !o->pending_perm.original_args) {
                free(o->pending_perm.id);
                free(o->pending_perm.original_args);
                o->pending_perm.id = NULL;
                o->pending_perm.original_args = NULL;
                tools_call_free(&call);
                free(effective_args);
                free(control_extension);
                free(control_reason);
                return -1;
            }
            o->pending_perm.call = call; /* transfer parsed args + summary */
            o->pending_perm.effective_args = effective_args;
            o->pending_perm.control_extension = control_extension;
            o->pending_perm.control_reason = control_reason;
            o->state = ST_WAIT_PERMISSION;

            tny_backend_event request = {0};
            request.kind = TNY_EV_PERMISSION;
            request.perm_id = o->pending_perm.id;
            request.perm_summary = o->pending_perm.call.summary;
            request.perm_options = TNY_PERM_ALLOW_ONCE |
                                   TNY_PERM_ALLOW_ALWAYS |
                                   TNY_PERM_DENY;
            emit(o, &request);
            return 0;
        }

        oa_pending_perm *p = &o->pending_perm;
        if (p->decision == TNY_PERM_DECISION_ALLOW_ALWAYS)
            tools_call_grant(&o->env, &p->call);
        if (p->decision == TNY_PERM_DECISION_DENY) {
            permission_block(o);
            char *result = tool_err("permission denied for %s", p->call.name);
            complete_tool(o, p->id, p->call.name, p->original_args,
                          p->effective_args, p->control_extension,
                          "user denied", result);
            free(result);
        } else {
            execute_call(o, p->id, p->original_args, p->effective_args,
                         p->control_extension, p->control_reason, &p->call);
        }
        pending_perm_clear(o);
        o->tool_index++;
    }
    return tny_alloc_scope_failed() ? -1 : finish_tool_batch(o);
}

static int step_finished(oa_impl *o) {
    tny_session_state *s = o->env.session;
    if (o->calls.n == 0) {
        if (o->steer && !o->cancelled) {
            /* the model answered before the steer could ride along: record
             * that answer and run one more round on the steered message so
             * it is addressed within the turn it targeted (adr/0011) */
            session_add_assistant(s, o->text.len ? o->text.data : "", NULL);
            take_steer(o);
            session_save(s);
            if (o->ctx->max_steps <= 0 || o->step + 1 < o->ctx->max_steps) {
                o->step++;
                char err[512];
                if (start_post(o, err, sizeof err) == 0) return 0;
                emit_error(o, TNY_EVENT_ERROR_IO, err, strlen(err));
                emit_turn_end(o, TNY_STOP_ERROR);
                return -1;
            }
        }
        finish_turn_ok(o);
        return 0;
    }

    /* Record the assistant batch once, then run it incrementally. An
     * unresolved permission may park the backend and resume here later. */
    if (!o->tool_batch_active) {
        buf_t tcj;
        buf_init(&tcj);
        buf_appends(&tcj, "[");
        char idbuf[16];
        for (int i = 0; i < o->calls.n; i++) {
            oa_call *pc = &o->calls.calls[i];
            if (i) buf_appends(&tcj, ",");
            buf_appendf(&tcj, "{\"id\":\"%s\",\"type\":\"function\",\"function\":{\"name\":\"%s\",\"arguments\":",
                        oa_call_id(pc, i, idbuf, sizeof idbuf),
                        pc->name ? pc->name : "unknown");
            jescape(&tcj, pc->args.data ? pc->args.data : "{}");
            buf_appends(&tcj, "}}");
        }
        buf_appends(&tcj, "]");
        session_add_assistant(s, o->text.len ? o->text.data : NULL, tcj.data);
        buf_free(&tcj);
        if (session_save(s) != 0) {
            emit_error(o, TNY_EVENT_ERROR_IO,
                       "could not persist proposed tool batch", 37);
            emit_turn_end(o, TNY_STOP_ERROR);
            return -1;
        }
        o->tool_batch_active = true;
        o->tool_index = 0;
    }
    return run_tools(o);
}

/* ---------- vtable ---------- */

static int oa_connect(tny_backend *b, char *errbuf, size_t errlen) {
    oa_impl *o = b->impl;
    if (!o->ctx->api_key &&
        !str_starts(o->ctx->base_url, "http://")) {
        const char *pn = o->ctx->provider_name;
        if (pn && strcmp(pn, "claude") == 0)
            snprintf(errbuf, errlen,
                     "no Claude credential: run `tny --provider claude login`, "
                     "or set CLAUDE_CODE_OAUTH_TOKEN / ANTHROPIC_API_KEY");
        else if (pn && strcmp(pn, "grok") == 0)
            snprintf(errbuf, errlen,
                     "no grok credential: run `tny --provider grok login` "
                     "(device auth), or set XAI_API_KEY");
        else
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
                   tny_backend_event_cb cb, void *ud, char *errbuf, size_t errlen) {
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
    pending_perm_clear(o);
    o->tool_batch_active = false;
    o->tool_index = 0;
    o->tool_batch_failed = 0;
    free(o->steer);
    o->steer = NULL;
    buf_clear(&o->toolcall_log);
    buf_appends(&o->toolcall_log, "[");
    oa_calls_reset(&o->calls);

    tny_session_state *s = o->env.session;
    session_set_meta(s, "openai", model_of(o));

    if (images && images[0]) {
        if (session_add_user_images(s, prompt, images, errbuf, errlen) != 0)
            return -1;
    } else {
        session_add_text(s, "user", prompt);
    }
    if (session_save(s) != 0) {
        snprintf(errbuf, errlen, "could not persist user prompt");
        return -1;
    }
    return start_post(o, errbuf, errlen);
}

/* Park text for the running turn; it is appended as a user message at the
 * next model-call boundary (after tool results), never into tool output. */
static int oa_steer(tny_backend *b, const char *text, char *errbuf, size_t errlen) {
    oa_impl *o = b->impl;
    if (o->state == ST_IDLE || o->cancelled) {
        snprintf(errbuf, errlen, "no turn is running");
        return -1;
    }
    if (o->steer) {
        /* two steers before a boundary: keep both, in order */
        size_t n = strlen(o->steer) + strlen(text) + 3;
        char *both = malloc(n);
        if (!both) { snprintf(errbuf, errlen, "out of memory"); return -1; }
        snprintf(both, n, "%s\n\n%s", o->steer, text);
        free(o->steer);
        o->steer = both;
        return 0;
    }
    o->steer = xstrdup(text);
    return 0;
}

static void oa_cancel(tny_backend *b) {
    oa_impl *o = b->impl;
    if (o->state == ST_IDLE) return;
    o->cancelled = true;
    bool had_tool_batch = o->tool_batch_active;
    if (had_tool_batch) {
        char idbuf[16];
        if (o->pending_perm.id) {
            oa_pending_perm *pending = &o->pending_perm;
            char *result = tool_err("interrupted before %s ran",
                                    pending->call.name);
            complete_tool(o, pending->id, pending->call.name,
                          pending->original_args, pending->effective_args,
                          pending->control_extension, "cancelled", result);
            free(result);
            pending_perm_clear(o);
            o->tool_index++;
        }
        for (int i = o->tool_index; i < o->calls.n; i++) {
            oa_call *pc = &o->calls.calls[i];
            finish_cancelled_call(o,
                oa_call_id(pc, i, idbuf, sizeof idbuf),
                pc->name ? pc->name : "unknown",
                pc->args.data ? pc->args.data : "{}");
        }
        o->tool_index = o->calls.n;
        oa_disconnect(b);
        (void)finish_tool_batch(o);
        return;
    }
    if (o->text.len && !had_tool_batch) {
        session_recovery_write(o->env.session, o->text.data);
        session_add_assistant(o->env.session, o->text.data, NULL);
        session_save(o->env.session);
    }
    oa_disconnect(b);
    emit_turn_end(o, TNY_STOP_INTERRUPTED);
}

static void oa_respond_permission(tny_backend *b, const char *id, tny_perm_decision d) {
    oa_impl *o = b->impl;
    if (!id || !o->pending_perm.id ||
        strcmp(id, o->pending_perm.id) != 0)
        return;
    o->pending_perm.decision = d;
    if (d == TNY_PERM_DECISION_DENY) permission_block(o);
    run_tools(o);
}

static int oa_pollfds(tny_backend *b, struct pollfd *fds, int max) {
    oa_impl *o = b->impl;
    if (o->state == ST_IDLE || o->state == ST_WAIT_PERMISSION ||
        !o->conn || max < 1) return 0;
    fds[0].fd = http_fd(o->conn);
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    return 1;
}

static int oa_dispatch(tny_backend *b, struct pollfd *fds, int n) {
    (void)fds; (void)n;
    oa_impl *o = b->impl;
    if (o->state == ST_IDLE || o->state == ST_WAIT_PERMISSION || !o->conn) return 0;

    if (o->state == ST_HEADERS) {
        int status = http_read_response(o->conn, 0);
        if (status == -2) return 0;
        if (status < 0) {
            if (o->conn_reused) {
                /* stale keep-alive caught at read time: the provider
                 * closed the idle connection after the previous response
                 * (SSE providers routinely do). No response byte arrived,
                 * so re-POST once on a fresh connection. */
                http_close(o->conn);
                o->conn = NULL;
                char rerr[512];
                tny_openai_control_response failed = provider_control(
                    o, TNY_OPENAI_CONTROL_PROVIDER_RESPONSE, 0);
                if (failed.stop) o->cancelled = true;
                control_response_free(&failed);
                if (start_post_mode(o, rerr, sizeof rerr, true) == 0) return 0;
                emit_error(o, TNY_EVENT_ERROR_IO, rerr, strlen(rerr));
                emit_turn_end(o, TNY_STOP_ERROR);
                return -1;
            }
            emit_error(o, TNY_EVENT_ERROR_IO,
                       "connection lost before response", 31);
            emit_turn_end(o, TNY_STOP_ERROR);
            return -1;
        }
        tny_openai_control_response control = provider_control(
            o, TNY_OPENAI_CONTROL_PROVIDER_RESPONSE, status);
        if (control.stop) {
            o->cancelled = true;
            control_response_free(&control);
            oa_disconnect(b);
            emit_turn_end(o, TNY_STOP_INTERRUPTED);
            return 0;
        }
        control_response_free(&control);
        if (status == 401 || status == 403) {
            emit_error(o, TNY_EVENT_ERROR_AUTH,
                       "authentication failed (401/403): check the API key", 51);
            emit_turn_end(o, TNY_STOP_ERROR);
            return -1;
        }
        if (status >= 400) {
            buf_t msg;
            buf_init(&msg);
            buf_appendf(&msg, "provider returned HTTP %d", status);
            emit_error(o, TNY_EVENT_ERROR_PROTOCOL, msg.data, msg.len);
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
        if (bn < 0) {
            /* A terminal SSE event can make the logical response complete
             * before an abruptly closed chunked body reports its truncated
             * transport.  That socket is known dead: retaining it makes the
             * tool-result POST depend on the OS eventually surfacing a stale
             * keep-alive read failure (tens of seconds on some macOS hosts).
             * Discard it now; step_finished opens a fresh connection. */
            oa_disconnect(b);
        }
        if (bn < 0 && !o->stream_done) {
            /* keep partial text recoverable */
            if (o->text.len) session_recovery_write(o->env.session, o->text.data);
            emit_error(o, TNY_EVENT_ERROR_IO,
                       "stream aborted mid-response", 27);
            emit_turn_end(o, TNY_STOP_ERROR);
            return -1;
        }
        if (o->stream_failed) {
            /* the provider ended the response with a terminal error event
             * (already surfaced): keep partial text recoverable, stop */
            if (o->text.len) session_recovery_write(o->env.session, o->text.data);
            emit_turn_end(o, TNY_STOP_ERROR);
            return -1;
        }
        return step_finished(o);
    }
}

static int oa_doctor(struct tny_ctx *ctx, char *line, size_t linelen) {
    const char *wire = tny_wire_is_chat(ctx->wire_api) ? ", wire chat" : "";
    if (ctx->api_key) {
        snprintf(line, linelen, "openai: key present, base_url %s%s",
                 ctx->base_url, wire);
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
    oa_calls_reset(&o->calls);
    pending_perm_clear(o);
    free(o->steer);
    buf_free(&o->text);
    buf_free(&o->toolcall_log);
    sse_parser_free(&o->sse);
    free(o);
    free(b);
}

void tny_backend_openai_bind(tny_backend *b, tny_session_state *session,
                             perm_engine *perm,
                             tny_perm_decision (*prompt)(const char *, const char *, void *),
                             void *prompt_ud,
                             tny_openai_control_cb control,
                             void *control_ud) {
    oa_impl *o = b->impl;
    o->env.session = session;
    o->env.perm = perm;
    o->env.prompt = prompt;
    o->env.prompt_ud = prompt_ud;
    o->control = control;
    o->control_ud = control_ud;
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
    yyjson_mut_doc *m = yyjson_mut_doc_new(jallocator());
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
    b->steer = oa_steer;
    b->cancel = oa_cancel;
    b->respond_permission = oa_respond_permission;
    b->pollfds = oa_pollfds;
    b->dispatch = oa_dispatch;
    b->doctor = oa_doctor;
    b->destroy = oa_destroy;
    return b;
}
