/* acp_client.c — `--backend acp`: spawn an ACP agent and run JSON-RPC 2.0 over
 * its stdio (transport, lifecycle, backend vtable). Message normalization
 * lives in acp_events.c. Contract: docs/backends/acp.md — protocolVersion 1,
 * session/prompt stays pending for the whole turn. */
#include "backends/acp/acp_client.h"
#include "util/util.h"

#include <poll.h>
#include "util/tny_poll.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void fail_turn(ac_impl *o, const char *msg) {
    if (!o->turn_active) return;
    ac_emit_text(o, TNY_EV_ERROR, msg, strlen(msg));
    ac_emit_end(o, TNY_STOP_ERROR);
}

/* ---------- vtable ---------- */

static void ac_disconnect(tny_backend *b) {
    ac_impl *o = b->impl;
    if (o->ws) { ws_close(o->ws); o->ws = NULL; }
    if (o->pid > 0) {
        pid_t pgid = o->pid; /* the spawn made the agent its group leader */
        if (o->in_fd >= 0) close(o->in_fd);
        o->in_fd = -1;
        int status = 0;
        for (int i = 0; i < 50; i++) {
            pid_t r = waitpid(o->pid, &status, WNOHANG);
            if (r == o->pid || r < 0) { o->pid = 0; break; }
            struct pollfd p = {o->out_fd, POLLIN, 0};
            tny_poll(&p, o->out_fd >= 0 ? 1 : 0, 10);
        }
        if (o->pid > 0) {
            if (kill(-pgid, SIGTERM) != 0) kill(o->pid, SIGTERM);
            waitpid(o->pid, &status, 0);
            o->pid = 0;
        }
        kill(-pgid, SIGKILL); /* sweep wrapper-forked descendants */
    }
    if (o->in_fd >= 0) { close(o->in_fd); o->in_fd = -1; }
    if (o->out_fd >= 0) { close(o->out_fd); o->out_fd = -1; }
    if (o->err_fd >= 0) { close(o->err_fd); o->err_fd = -1; }
}

static int ac_connect(tny_backend *b, char *errbuf, size_t errlen) {
    ac_impl *o = b->impl;
    if (o->pid > 0 || o->ws) return 0;
    if (ac_agent_is_ws(o->ctx->agent_argv ? o->ctx->agent_argv[0] : NULL)) {
        if (ac_connect_ws(o, errbuf, errlen) != 0) return -1;
    } else if (ac_spawn_agent(o, errbuf, errlen) != 0) {
        return -1;
    }

    buf_t p;
    buf_init(&p);
    buf_appendf(&p,
        "{\"protocolVersion\":%d,\"clientCapabilities\":{\"fs\":{\"readTextFile\":false,"
        "\"writeTextFile\":false},\"terminal\":false},\"clientInfo\":{\"name\":\"tny\","
        "\"title\":\"tny\",\"version\":\"%s\"}}",
        ACP_PROTOCOL_VERSION, TNY_VERSION);
    yyjson_doc *doc = ac_rpc(o, "initialize", p.data, errbuf, errlen);
    buf_free(&p);
    if (!doc) { ac_disconnect(b); return -1; }

    yyjson_val *res = jget(yyjson_doc_get_root(doc), "result");
    int64_t ver = jget_int(res, "protocolVersion", ACP_PROTOCOL_VERSION);
    o->load_session = jget_bool(jget(res, "agentCapabilities"), "loadSession", false);
    yyjson_val *auth = jget(res, "authMethods");
    bool needs_auth = auth && yyjson_is_arr(auth) && yyjson_arr_size(auth) > 0;
    yyjson_doc_free(doc);

    if (ver != ACP_PROTOCOL_VERSION) {
        snprintf(errbuf, errlen,
                 "acp: agent negotiated protocolVersion %lld; tny speaks %d",
                 (long long)ver, ACP_PROTOCOL_VERSION);
        ac_disconnect(b);
        return -1;
    }
    if (needs_auth)
        fprintf(stderr, "acp: agent advertises authMethods; tny expects a "
                        "pre-authenticated agent\n");
    return 0;
}

static int ac_create_or_resume(tny_backend *b, const char *ptr, char *e, size_t el) {
    ac_impl *o = b->impl;
    buf_t p;
    buf_init(&p);
    yyjson_doc *doc = NULL;

    if (ptr && *ptr && o->load_session) {
        buf_appends(&p, "{\"sessionId\":");
        jescape(&p, ptr);
        buf_appends(&p, ",\"cwd\":");
        jescape(&p, o->ctx->cwd);
        buf_appends(&p, ",\"mcpServers\":[]}");
        doc = ac_rpc(o, "session/load", p.data, e, el);
        if (doc) {
            yyjson_doc_free(doc);
            free(o->session_id);
            o->session_id = xstrdup(ptr);
            buf_free(&p);
            return 0;
        }
        /* may run on the TUI pre-warm thread: diagnostics stay off the tty */
        if (tny_debug())
            fprintf(stderr, "acp: session/load failed (%s); starting a new session\n", e);
    } else if (ptr && *ptr) {
        if (tny_debug())
            fprintf(stderr, "acp: agent has no loadSession capability; "
                            "starting a new session\n");
    }

    buf_clear(&p);
    buf_appends(&p, "{\"cwd\":");
    jescape(&p, o->ctx->cwd);
    buf_appends(&p, ",\"mcpServers\":[]}");
    doc = ac_rpc(o, "session/new", p.data, e, el);
    buf_free(&p);
    if (!doc) return -1;
    const char *sid = jget_str(jget(yyjson_doc_get_root(doc), "result"), "sessionId");
    if (!sid) {
        snprintf(e, el, "acp: session/new returned no sessionId");
        yyjson_doc_free(doc);
        return -1;
    }
    free(o->session_id);
    o->session_id = xstrdup(sid);
    yyjson_doc_free(doc);
    return 0;
}

static char *ac_session_pointer(tny_backend *b) {
    ac_impl *o = b->impl;
    return o->session_id ? xstrdup(o->session_id) : NULL;
}

static int ac_send(tny_backend *b, const char *prompt, const char **images,
                   tny_event_cb cb, void *ud, char *errbuf, size_t errlen) {
    ac_impl *o = b->impl;
    if (!o->session_id) {
        snprintf(errbuf, errlen, "acp: no session (call create_or_resume first)");
        return -1;
    }
    if (images && images[0]) {
        snprintf(errbuf, errlen,
                 "acp: image prompts are not supported by the ACP client backend");
        return -1;
    }
    o->cb = cb;
    o->ud = ud;
    o->cancelled = false;
    ac_perms_clear(o);
    if (o->ctx->reasoning_effort && *o->ctx->reasoning_effort && !o->effort_noted) {
        /* ACP has no portable reasoning-effort knob at protocolVersion 1;
         * agent-specific config options are a later addition. Say so once
         * instead of silently running at whatever the agent defaults to. */
        static const char m[] = "acp: reasoning effort is not forwarded to ACP "
                                "agents; the agent's own default applies";
        ac_emit_text(o, TNY_EV_STATUS, m, sizeof m - 1);
        o->effort_noted = true;
    }

    buf_t p;
    buf_init(&p);
    buf_appends(&p, "{\"sessionId\":");
    jescape(&p, o->session_id);
    buf_appends(&p, ",\"prompt\":[");
    acp_append_text_block(&p, prompt, strlen(prompt));
    buf_appends(&p, "]}");

    o->prompt_id = o->next_id++;
    o->turn_active = true;
    int rc = ac_tx_request(o, o->prompt_id, "session/prompt", p.data);
    buf_free(&p);
    if (rc != 0) {
        o->turn_active = false;
        snprintf(errbuf, errlen, "acp: agent closed its input");
        return -1;
    }
    return 0;
}

static void ac_cancel(tny_backend *b) {
    ac_impl *o = b->impl;
    if (!o->turn_active || o->cancelled) return;
    o->cancelled = true;
    buf_t p;
    buf_init(&p);
    buf_appends(&p, "{\"sessionId\":");
    jescape(&p, o->session_id ? o->session_id : "");
    buf_appends(&p, "}");
    ac_tx_notify(o, "session/cancel", p.data);
    buf_free(&p);
    /* Answer everything still pending so the agent can unwind. */
    while (o->nperms) {
        ac_tx_result(o, o->perms[0].id_raw,
                     "{\"outcome\":{\"outcome\":\"cancelled\"}}");
        ac_perm_drop(o, &o->perms[0]);
    }
}

static void ac_respond_permission(tny_backend *b, const char *perm_id,
                                  tny_perm_decision d) {
    ac_impl *o = b->impl;
    if (!perm_id) return;
    ac_perm *p = ac_perm_find(o, perm_id);
    if (!p) return;
    const char *choice = NULL;
    if (o->cancelled) {
        ac_tx_result(o, p->id_raw, "{\"outcome\":{\"outcome\":\"cancelled\"}}");
        ac_perm_drop(o, p);
        return;
    }
    switch (d) {
    case TNY_PERM_DECISION_ALLOW_ALWAYS:
        choice = p->allow_always ? p->allow_always : p->allow_once;
        break;
    case TNY_PERM_DECISION_ALLOW:
        choice = p->allow_once ? p->allow_once : p->allow_always;
        break;
    default:
        choice = p->reject;
        break;
    }
    if (!choice) {
        /* The agent offered nothing usable for this decision: cancel instead
         * of guessing an option that might approve something. */
        ac_tx_result(o, p->id_raw, "{\"outcome\":{\"outcome\":\"cancelled\"}}");
        ac_perm_drop(o, p);
        return;
    }
    buf_t r;
    buf_init(&r);
    buf_appends(&r, "{\"outcome\":{\"outcome\":\"selected\",\"optionId\":");
    jescape(&r, choice);
    buf_appends(&r, "}}");
    ac_tx_result(o, p->id_raw, r.data);
    buf_free(&r);
    ac_perm_drop(o, p);
}

static int ac_pollfds(tny_backend *b, struct pollfd *fds, int max) {
    ac_impl *o = b->impl;
    return ac_transport_pollfds(o, fds, max);
}

static int ac_dispatch(tny_backend *b, struct pollfd *fds, int n) {
    (void)fds; (void)n;
    ac_impl *o = b->impl;
    if (!o->ws && o->out_fd < 0) return 0;
    int rc = ac_pump_reads(o);
    if (rc == -2) {
        fail_turn(o, "acp: agent sent a message over the 8 MiB cap");
        return -1;
    }
    if (rc == -1) {
        /* stdout closed: give the child a moment to land its exit status so
         * the turn error can name it. */
        int code = o->ws ? -1 : ac_reap_agent(o);
        if (o->turn_active) {
            char msg[160];
            if (code == 127)
                snprintf(msg, sizeof msg, "acp: agent '%s' could not be executed",
                         o->ctx->agent_argv && o->ctx->agent_argv[0]
                             ? o->ctx->agent_argv[0] : "?");
            else if (code >= 0)
                snprintf(msg, sizeof msg, "acp: agent exited (status %d) mid-turn", code);
            else
                snprintf(msg, sizeof msg, "acp: agent closed the connection mid-turn");
            fail_turn(o, msg);
        }
        return -1;
    }
    return 0;
}

static int ac_doctor(struct tny_ctx *ctx, char *line, size_t linelen) {
    if (!ctx->agent_argv || !ctx->agent_argv[0]) {
        snprintf(line, linelen,
                 "acp: no agent configured (tny --provider acp --agent CMD -- args…)");
        return 1;
    }
    if (ac_agent_is_ws(ctx->agent_argv[0])) {
        snprintf(line, linelen, "acp: remote agent %.80s (WebSocket)",
                 ctx->agent_argv[0]);
        return 0;
    }
    if (!ac_on_path(ctx->agent_argv[0])) {
        snprintf(line, linelen, "acp: agent '%.80s' not found on PATH",
                 ctx->agent_argv[0]);
        return 1;
    }
    snprintf(line, linelen, "acp: agent '%.80s' resolves", ctx->agent_argv[0]);
    return 0;
}

static void ac_destroy(tny_backend *b) {
    ac_impl *o = b->impl;
    ac_disconnect(b);
    ac_perms_clear(o);
    acp_reader_free(&o->out_r);
    acp_reader_free(&o->err_r);
    free(o->session_id);
    if (o->wait_doc) yyjson_doc_free(o->wait_doc);
    free(o);
    free(b);
}

tny_backend *tny_backend_acp_new(struct tny_ctx *ctx) {
    tny_backend *b = calloc(1, sizeof *b);
    ac_impl *o = calloc(1, sizeof *o);
    if (!b || !o) { free(b); free(o); return NULL; }
    o->ctx = ctx;
    o->in_fd = o->out_fd = o->err_fd = -1;
    o->next_id = 1;
    o->wait_id = -1;
    o->prompt_id = -1;
    acp_reader_init(&o->out_r);
    acp_reader_init(&o->err_r);
    b->id = TNY_BK_ACP;
    b->impl = o;
    b->connect = ac_connect;
    b->disconnect = ac_disconnect;
    b->create_or_resume = ac_create_or_resume;
    b->session_pointer = ac_session_pointer;
    b->send = ac_send;
    b->cancel = ac_cancel;
    b->respond_permission = ac_respond_permission;
    b->pollfds = ac_pollfds;
    b->dispatch = ac_dispatch;
    b->doctor = ac_doctor;
    b->destroy = ac_destroy;
    return b;
}
