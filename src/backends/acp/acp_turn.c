/* acp_turn.c — running one `session/prompt` for the `tny acp` server: normalized
 * events out as session/update, permission prompts out as
 * session/request_permission (docs/backends/acp.md). */
#include "backends/acp/acp_server.h"
#include "util/tny_poll.h"
#include "backends/openai/openai.h"
#include "util/util.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACP_TOOL_TEXT_CAP 4000
#define ACP_RAW_INPUT_CAP 8192

static void send_update(acp_srv *s, const char *update_json) {
    if (!s->session_id) return;
    buf_t p;
    buf_init(&p);
    buf_appends(&p, "{\"sessionId\":");
    jescape(&p, s->session_id);
    buf_appendf(&p, ",\"update\":%s}", update_json);
    acp_send_notify(s->out_fd, "session/update", p.data);
    buf_free(&p);
}

static void send_chunk(acp_srv *s, const char *kind, const char *text, size_t len) {
    if (!text || !len) return;
    buf_t u;
    buf_init(&u);
    buf_appendf(&u, "{\"sessionUpdate\":\"%s\",\"content\":", kind);
    acp_append_text_block(&u, text, len);
    buf_appends(&u, "}");
    send_update(s, u.data);
    buf_free(&u);
}

/* ACP ToolCallKind for a tny built-in tool. */
static const char *tool_kind(const char *name) {
    if (!name) return "other";
    if (!strcmp(name, "read_file") || !strcmp(name, "read_image") ||
        !strcmp(name, "list_files") || !strcmp(name, "file_info") ||
        !strcmp(name, "read_tool_result"))
        return "read";
    if (!strcmp(name, "glob_files") || !strcmp(name, "grep_files") ||
        !strcmp(name, "semantic_search"))
        return "search";
    if (!strcmp(name, "write_file") || !strcmp(name, "edit_file") ||
        !strcmp(name, "create_folder") || !strcmp(name, "copy_file"))
        return "edit";
    if (!strcmp(name, "delete_file")) return "delete";
    if (!strcmp(name, "rename_file")) return "move";
    if (!strcmp(name, "terminal") || !strcmp(name, "run_command")) return "execute";
    if (!strcmp(name, "web_fetch") || !strcmp(name, "web_search")) return "fetch";
    if (!strcmp(name, "skill") || !strcmp(name, "subagent") || !strcmp(name, "memory"))
        return "think";
    return "other";
}

/* Append `"rawInput":<obj>` when the recorded arguments really are an object. */
static void append_raw_input(buf_t *u, const char *args_json) {
    if (!args_json || !*args_json) return;
    size_t len = strlen(args_json);
    if (len > ACP_RAW_INPUT_CAP) return;
    yyjson_doc *doc = jparse(args_json, len);
    if (!doc) return;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root && yyjson_is_obj(root)) {
        char *compact = jwrite_val(root);
        if (compact) {
            buf_appendf(u, ",\"rawInput\":%s", compact);
            free(compact);
        }
    }
    yyjson_doc_free(doc);
}

static void tool_start(acp_srv *s, const tny_backend_event *ev) {
    buf_t u;
    buf_init(&u);
    buf_appends(&u, "{\"sessionUpdate\":\"tool_call\",\"toolCallId\":");
    jescape(&u, ev->tool_id ? ev->tool_id : "call_0");
    buf_appends(&u, ",\"title\":");
    jescape(&u, ev->tool_name ? ev->tool_name : "tool");
    buf_appendf(&u, ",\"kind\":\"%s\",\"status\":\"in_progress\"", tool_kind(ev->tool_name));
    append_raw_input(&u, ev->tool_detail);
    buf_appends(&u, "}");
    send_update(s, u.data);
    buf_free(&u);
}

static void tool_end(acp_srv *s, const tny_backend_event *ev) {
    buf_t u;
    buf_init(&u);
    buf_appends(&u, "{\"sessionUpdate\":\"tool_call_update\",\"toolCallId\":");
    jescape(&u, ev->tool_id ? ev->tool_id : "call_0");
    buf_appendf(&u, ",\"status\":\"%s\"", ev->tool_ok ? "completed" : "failed");
    if (ev->tool_detail && *ev->tool_detail) {
        size_t n = strlen(ev->tool_detail);
        if (n > ACP_TOOL_TEXT_CAP) n = ACP_TOOL_TEXT_CAP;
        buf_appends(&u, ",\"content\":[{\"type\":\"content\",\"content\":");
        acp_append_text_block(&u, ev->tool_detail, n);
        buf_appends(&u, "}]");
    }
    buf_appends(&u, "}");
    send_update(s, u.data);
    buf_free(&u);
}

static void plan_update(acp_srv *s, const tny_backend_event *ev) {
    buf_t u;
    buf_init(&u);
    buf_appends(&u, "{\"sessionUpdate\":\"plan\",\"entries\":[{\"content\":");
    char *text = xstrndup(ev->text, ev->text_len);
    jescape(&u, text);
    free(text);
    buf_appends(&u, ",\"priority\":\"medium\",\"status\":\"pending\"}]}");
    send_update(s, u.data);
    buf_free(&u);
}

static void srv_event_cb(const tny_backend_event *ev, void *ud) {
    acp_srv *s = ud;
    switch (ev->kind) {
    case TNY_EV_TEXT_DELTA:
        send_chunk(s, "agent_message_chunk", ev->text, ev->text_len);
        break;
    case TNY_EV_THINKING:
        send_chunk(s, "agent_thought_chunk", ev->text, ev->text_len);
        break;
    case TNY_EV_TOOL_START:
        tool_start(s, ev);
        break;
    case TNY_EV_TOOL_END:
        tool_end(s, ev);
        break;
    case TNY_EV_PLAN:
        plan_update(s, ev);
        break;
    case TNY_EV_USAGE:
        acp_srv_log("tokens: %lld in, %lld out", (long long)ev->in_tokens,
                    (long long)ev->out_tokens);
        break;
    case TNY_EV_STATUS:
        acp_srv_log("%.*s", (int)ev->text_len, ev->text);
        break;
    case TNY_EV_STEER_REJECTED: /* the acp server never steers its loop */
        break;
    case TNY_EV_ERROR:
        buf_clear(&s->last_error);
        buf_append(&s->last_error, ev->text, ev->text_len);
        acp_srv_log("error: %.*s", (int)ev->text_len, ev->text);
        break;
    case TNY_EV_PERMISSION:
        /* the native loop resolves approvals through the prompt hook */
        break;
    case TNY_EV_TURN_END:
        s->turn_done = true;
        s->stop = ev->stop;
        break;
    }
}

/* ---------- approvals ---------- */

tny_perm_decision acp_srv_prompt(const char *tool, const char *summary, void *ud) {
    acp_srv *s = ud;
    if (s->cancel_requested) return TNY_PERM_DECISION_DENY;

    s->perm_req_id = s->next_id++;
    s->perm_waiting = true;
    s->perm_answered = false;
    s->perm_result = TNY_PERM_DECISION_DENY;

    buf_t p;
    buf_init(&p);
    buf_appends(&p, "{\"sessionId\":");
    jescape(&p, s->session_id ? s->session_id : "");
    buf_appendf(&p, ",\"toolCall\":{\"toolCallId\":\"perm-%lld\",\"title\":",
                (long long)s->perm_req_id);
    jescape(&p, summary && *summary ? summary : (tool ? tool : "tool call"));
    buf_appendf(&p, ",\"kind\":\"%s\",\"status\":\"pending\"}", tool_kind(tool));
    buf_appends(&p, ",\"options\":["
        "{\"optionId\":\"allow\",\"name\":\"Allow once\",\"kind\":\"allow_once\"},"
        "{\"optionId\":\"allow-always\",\"name\":\"Allow for this session\","
        "\"kind\":\"allow_always\"},"
        "{\"optionId\":\"reject\",\"name\":\"Reject\",\"kind\":\"reject_once\"}]}");
    int rc = acp_send_request(s->out_fd, s->perm_req_id, "session/request_permission",
                              p.data);
    buf_free(&p);
    if (rc != 0) {
        s->perm_waiting = false;
        s->eof = true;
        return TNY_PERM_DECISION_DENY;
    }
    /* Block this tool — and only this tool — until the client answers. */
    while (!s->perm_answered) {
        if (acp_srv_pump(s, 200) != 0) break;
        if (s->cancel_requested) break;
    }
    s->perm_waiting = false;
    if (!s->perm_answered) return TNY_PERM_DECISION_DENY;
    return s->perm_result;
}

static void drain_engine_events(acp_srv *s) {
    tny_owned_event *owned;
    while ((owned = tny_engine_pop_event(s->engine))) {
        srv_event_cb(&owned->ev, s);
        tny_owned_event_free(owned);
    }
}

/* ---------- the turn ---------- */

static const char *reason_of(tny_stop_reason stop) {
    switch (stop) {
    case TNY_STOP_DONE:        return "end_turn";
    case TNY_STOP_INTERRUPTED: return "cancelled";
    case TNY_STOP_DENIED:      return "refusal";
    case TNY_STOP_STEP_LIMIT:  return "max_turn_requests";
    default:                   return "refusal";
    }
}

int acp_srv_run_turn(acp_srv *s, const char *text, const char **reason) {
    if (!s->engine || !s->session) {
        buf_clear(&s->last_error);
        buf_appends(&s->last_error, "no active session");
        return -1;
    }
    s->turn_active = true;
    s->turn_done = false;
    s->cancelled = false;
    s->cancel_requested = false;
    s->stop = TNY_STOP_DONE;
    buf_clear(&s->last_error);

    char err[512];
    if (tny_engine_start(s->engine, text, NULL, err, sizeof err) != 0) {
        s->turn_active = false;
        buf_clear(&s->last_error);
        buf_appends(&s->last_error, err);
        return -1;
    }
    drain_engine_events(s);

    while (!s->turn_done) {
        if (acp_srv_pump(s, 0) != 0) {
            tny_engine_cancel(s->engine);
            drain_engine_events(s);
            break;
        }
        if (s->cancel_requested && !s->cancelled) {
            s->cancelled = true;
            tny_engine_cancel(s->engine);
            drain_engine_events(s);
            continue;
        }
        struct pollfd fds[8];
        int n = tny_engine_pollfds(s->engine, fds, 8);
        if (n > 0) {
            tny_poll(fds, (nfds_t)n, 50);
        } else {
            struct pollfd idle = {s->in_fd, POLLIN, 0};
            tny_poll(&idle, 1, 20);
        }
        tny_engine_dispatch(s->engine, fds, n);
        drain_engine_events(s);
    }
    s->turn_active = false;
    if (s->cancelled && s->stop == TNY_STOP_DONE) s->stop = TNY_STOP_INTERRUPTED;
    *reason = reason_of(s->stop);
    if (s->eof) return -1;
    return 0;
}
