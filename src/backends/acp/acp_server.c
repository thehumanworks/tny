/* acp_server.c — `tny acp`: serve the native OpenAI-compatible loop to an ACP
 * client over stdio (docs/backends/acp.md, fx parity).
 * stdout carries protocol JSON only; every log line goes to stderr. */
#include "backends/acp/acp_server.h"
#include "cli/cli.h"
#include "mcp/mcp.h"
#include "util/util.h"
#include "util/tny_poll.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void acp_srv_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("tny acp: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---------- session lifecycle ---------- */

static void drop_session(acp_srv *s) {
    if (s->engine) {
        tny_engine_free(s->engine);
        s->engine = NULL;
    }
    if (s->perm) {
        perm_free(s->perm);
        s->perm = NULL;
    }
    if (s->session) {
        session_close(s->session);
        s->session = NULL;
    }
    free(s->session_id);
    s->session_id = NULL;
}

static bool acp_srv_cancel_probe(void *ud) { return ((acp_srv *)ud)->cancel_requested; }

/* Attach a fresh native backend to `sess`. Returns an error string or NULL. */
static const char *attach_backend(acp_srv *s, tny_session_state *sess, char *err, size_t errlen) {
    drop_session(s);
    s->session = sess;
    s->session_id = xstrdup(sess->id);
    s->perm = perm_new(s->ctx);
    s->engine = tny_engine_new(s->ctx, s->session, s->perm, acp_srv_prompt, s);
    if (!s->engine) return "cannot create the native runtime";
    tny_engine_set_cancel_probe(s->engine, acp_srv_cancel_probe, s);
    tny_backend *bk = tny_backend_create(TNY_BK_OPENAI, s->ctx);
    if (!bk) return "cannot create the native backend";
    if (tny_engine_prepare(s->engine, bk, TNY_ENGINE_PREPARE_FRESH, err, errlen) != 0) return err;
    session_set_meta(sess, "openai", s->ctx->model ? s->ctx->model : "default");
    return NULL;
}

/* ---------- request handlers ---------- */

static bool have_credential(tny_ctx *ctx) {
    return ctx->api_key != NULL || str_starts(ctx->base_url, "http://");
}

static void handle_initialize(acp_srv *s, const char *id, yyjson_val *params) {
    int64_t ver = jget_int(params, "protocolVersion", ACP_PROTOCOL_VERSION);
    if (ver < ACP_PROTOCOL_VERSION) {
        acp_send_error(s->out_fd, id, ACP_E_INVALID, "tny speaks ACP protocolVersion 1");
        return;
    }
    if (!have_credential(s->ctx)) {
        buf_t m;
        buf_init(&m);
        buf_appendf(&m,
                    "no provider credential: set OPENAI_API_KEY (base_url %s). "
                    "Run `tny setup` or `tny doctor` first.",
                    s->ctx->base_url);
        acp_send_error(s->out_fd, id, ACP_E_AUTH, m.data);
        buf_free(&m);
        return;
    }
    buf_t r;
    buf_init(&r);
    buf_appendf(&r,
                "{\"protocolVersion\":%d,\"agentCapabilities\":{\"loadSession\":%s,"
                "\"promptCapabilities\":{\"image\":false,\"audio\":false,"
                "\"embeddedContext\":true}},\"authMethods\":[],"
                "\"agentInfo\":{\"name\":\"tny\",\"title\":\"tny\",\"version\":\"%s\"}}",
                ACP_PROTOCOL_VERSION, s->ctx->no_save ? "false" : "true", TNY_VERSION);
    acp_send_result(s->out_fd, id, r.data);
    buf_free(&r);
    s->initialized = true;
}

static bool require_init(acp_srv *s, const char *id) {
    if (s->initialized) return true;
    acp_send_error(s->out_fd, id, ACP_E_INVALID, "initialize must come first");
    return false;
}

static void warn_client_mcp(yyjson_val *params) {
    yyjson_val *mcp = jget(params, "mcpServers");
    if (mcp && yyjson_is_arr(mcp) && yyjson_arr_size(mcp) > 0)
        acp_srv_log("ignoring %zu client mcpServers: MCP bridging is not "
                    "implemented in this build",
                    yyjson_arr_size(mcp));
}

static void handle_new(acp_srv *s, const char *id, yyjson_val *params) {
    if (!require_init(s, id)) return;
    const char *cwd = jget_str(params, "cwd");
    if (cwd && strcmp(cwd, s->ctx->cwd) != 0)
        acp_srv_log("client asked for cwd %s; serving the launch workspace %s", cwd, s->ctx->cwd);
    warn_client_mcp(params);

    tny_session_state *sess = session_new(s->ctx);
    if (!sess) {
        acp_send_error(s->out_fd, id, ACP_E_INTERNAL, "cannot create a session");
        return;
    }
    char err[256];
    const char *e = attach_backend(s, sess, err, sizeof err);
    if (e) {
        acp_send_error(s->out_fd, id, ACP_E_INTERNAL, e);
        drop_session(s);
        return;
    }
    session_save(sess);
    buf_t r;
    buf_init(&r);
    buf_appends(&r, "{\"sessionId\":");
    jescape(&r, s->session_id);
    buf_appends(&r, "}");
    acp_send_result(s->out_fd, id, r.data);
    buf_free(&r);
}

/* Replay the stored transcript as session/update chunks (v1 session/load
 * contract: history first, then the response). */
static void replay_history(acp_srv *s) {
    yyjson_mut_val *msgs = session_messages(s->session);
    size_t i, max;
    yyjson_mut_val *m;
    yyjson_mut_arr_foreach(msgs, i, max, m) {
        if (!m) break;
        yyjson_mut_val *rv = yyjson_mut_obj_get(m, "role");
        yyjson_mut_val *cv = yyjson_mut_obj_get(m, "content");
        const char *role = yyjson_mut_get_str(rv);
        const char *text = yyjson_mut_get_str(cv);
        if (!role || !text || !*text) continue;
        const char *kind = NULL;
        if (strcmp(role, "user") == 0) kind = "user_message_chunk";
        else if (strcmp(role, "assistant") == 0) kind = "agent_message_chunk";
        if (!kind) continue;
        buf_t u;
        buf_init(&u);
        buf_appendf(&u, "{\"sessionUpdate\":\"%s\",\"content\":", kind);
        acp_append_text_block(&u, text, strlen(text));
        buf_appends(&u, "}");
        buf_t p;
        buf_init(&p);
        buf_appends(&p, "{\"sessionId\":");
        jescape(&p, s->session_id);
        buf_appendf(&p, ",\"update\":%s}", u.data);
        acp_send_notify(s->out_fd, "session/update", p.data);
        buf_free(&p);
        buf_free(&u);
    }
}

static void handle_load(acp_srv *s, const char *id, yyjson_val *params) {
    if (!require_init(s, id)) return;
    if (s->ctx->no_save) {
        acp_send_error(s->out_fd, id, ACP_E_NO_METHOD,
                       "session/load is unavailable in ephemeral mode");
        return;
    }
    const char *sid = jget_str(params, "sessionId");
    if (!sid || !*sid) {
        acp_send_error(s->out_fd, id, ACP_E_PARAMS, "sessionId is required");
        return;
    }
    warn_client_mcp(params);
    tny_session_state *sess = session_open(s->ctx, sid);
    if (!sess) {
        acp_send_error(s->out_fd, id, ACP_E_PARAMS, "no such session in this workspace");
        return;
    }
    char err[256];
    const char *e = attach_backend(s, sess, err, sizeof err);
    if (e) {
        acp_send_error(s->out_fd, id, ACP_E_INTERNAL, e);
        drop_session(s);
        return;
    }
    replay_history(s);
    acp_send_result(s->out_fd, id, "null");
}

static void handle_prompt(acp_srv *s, const char *id, yyjson_val *params) {
    if (!require_init(s, id)) return;
    const char *sid = jget_str(params, "sessionId");
    if (!s->session || !s->session_id || (sid && strcmp(sid, s->session_id) != 0)) {
        acp_send_error(s->out_fd, id, ACP_E_PARAMS, "unknown sessionId");
        return;
    }
    if (s->turn_active) {
        acp_send_error(s->out_fd, id, ACP_E_INTERNAL,
                       "a prompt is already running on this connection");
        return;
    }
    buf_t text;
    buf_init(&text);
    const char *bad = NULL;
    if (!acp_blocks_to_text(jget(params, "prompt"), &text, &bad)) {
        buf_t m;
        buf_init(&m);
        buf_appendf(&m,
                    "content block type '%.40s' is not supported by tny acp "
                    "(text and embedded resources only)",
                    bad ? bad : "?");
        acp_send_error(s->out_fd, id, ACP_E_PARAMS, m.data);
        buf_free(&m);
        buf_free(&text);
        return;
    }
    if (!text.len) {
        acp_send_error(s->out_fd, id, ACP_E_PARAMS, "prompt has no text content");
        buf_free(&text);
        return;
    }
    const char *reason = "end_turn";
    int rc = acp_srv_run_turn(s, text.data, &reason);
    buf_free(&text);
    if (rc != 0) {
        if (s->eof) return; /* client vanished mid-turn */
        acp_send_error(s->out_fd, id, ACP_E_INTERNAL,
                       s->last_error.len ? s->last_error.data : "turn failed");
        return;
    }

    buf_t r;
    buf_init(&r);
    buf_appends(&r, "{\"stopReason\":");
    jescape(&r, reason);
    buf_appends(&r, "}");
    acp_send_result(s->out_fd, id, r.data);
    buf_free(&r);
}

static void handle_set_mode(acp_srv *s, const char *id, yyjson_val *params) {
    const char *mode = jget_str(params, "modeId");
    if (!mode) mode = jget_str(params, "mode");
    if (!mode) {
        acp_send_error(s->out_fd, id, ACP_E_PARAMS, "modeId is required");
        return;
    }
    /* fx modes: ask (approve sensitive tools), code (auto-review). */
    if (strcmp(mode, "ask") == 0) s->ctx->perm_mode = TNY_MODE_ASK;
    else if (strcmp(mode, "code") == 0 || strcmp(mode, "auto") == 0)
        s->ctx->perm_mode = TNY_MODE_AUTO;
    else if (strcmp(mode, "yolo") == 0 || strcmp(mode, "bypassPermissions") == 0)
        s->ctx->perm_mode = TNY_MODE_YOLO;
    else {
        acp_send_error(s->out_fd, id, ACP_E_PARAMS, "modeId must be ask|code|yolo");
        return;
    }
    acp_srv_log("permission mode set to %s", tny_perm_mode_name(s->ctx->perm_mode));
    acp_send_result(s->out_fd, id, "null");
}

/* ---------- dispatch ---------- */

static void handle_request(acp_srv *s, const char *id, const char *method, yyjson_val *params) {
    if (strcmp(method, "initialize") == 0) handle_initialize(s, id, params);
    else if (strcmp(method, "authenticate") == 0) acp_send_result(s->out_fd, id, "null");
    else if (strcmp(method, "session/new") == 0) handle_new(s, id, params);
    else if (strcmp(method, "session/load") == 0) handle_load(s, id, params);
    else if (strcmp(method, "session/prompt") == 0) handle_prompt(s, id, params);
    else if (strcmp(method, "session/set_mode") == 0) handle_set_mode(s, id, params);
    else if (strcmp(method, "session/set_config_option") == 0)
        acp_send_result(s->out_fd, id, "null");
    else if (strcmp(method, "session/close") == 0) {
        drop_session(s);
        acp_send_result(s->out_fd, id, "null");
    } else {
        buf_t m;
        buf_init(&m);
        buf_appendf(&m, "unknown method '%.100s'", method);
        acp_send_error(s->out_fd, id, ACP_E_NO_METHOD, m.data);
        buf_free(&m);
    }
}

static void handle_response(acp_srv *s, yyjson_val *msg) {
    if (!s->perm_waiting || acp_id_num(msg) != s->perm_req_id) return;
    yyjson_val *res = jget(msg, "result");
    if (!res) { /* the client errored out: fail closed */
        s->perm_result = TNY_PERM_DECISION_DENY;
        s->perm_answered = true;
        return;
    }
    yyjson_val *out = jget(res, "outcome");
    const char *outcome = jget_str(out, "outcome");
    const char *opt = jget_str(out, "optionId");
    if (outcome && strcmp(outcome, "selected") == 0 && opt) {
        if (strcmp(opt, "allow") == 0) s->perm_result = TNY_PERM_DECISION_ALLOW;
        else if (strcmp(opt, "allow-always") == 0) s->perm_result = TNY_PERM_DECISION_ALLOW_ALWAYS;
        else s->perm_result = TNY_PERM_DECISION_DENY;
    } else {
        if (outcome && strcmp(outcome, "cancelled") == 0) s->cancel_requested = true;
        s->perm_result = TNY_PERM_DECISION_DENY;
    }
    s->perm_answered = true;
}

static void handle_message(acp_srv *s, yyjson_val *msg) {
    if (!msg || !yyjson_is_obj(msg)) return;
    const char *method = jget_str(msg, "method");
    if (!method) {
        handle_response(s, msg);
        return;
    }
    yyjson_val *params = jget(msg, "params");

    if (!jget(msg, "id")) { /* notification */
        if (strcmp(method, "session/cancel") == 0) {
            s->cancel_requested = true;
            if (!s->turn_active) acp_srv_log("session/cancel with no active prompt");
        }
        return;
    }
    char *id = acp_id_text(msg);
    if (s->turn_active) {
        acp_send_error(s->out_fd, id, ACP_E_INTERNAL, "tny acp serves one prompt at a time");
    } else {
        handle_request(s, id, method, params);
    }
    free(id);
}

int acp_srv_pump(acp_srv *s, int timeout_ms) {
    struct pollfd p = {s->in_fd, POLLIN, 0};
    int pr = tny_poll(&p, 1, timeout_ms);
    if (pr < 0) return (errno == EINTR) ? 0 : -1;
    if (pr > 0) {
        char tmp[16384];
        ssize_t n = read(s->in_fd, tmp, sizeof tmp);
        if (n == 0) {
            s->eof = true;
            return -1;
        }
        if (n < 0 && errno != EINTR && errno != EAGAIN) {
            s->eof = true;
            return -1;
        }
        if (n > 0) acp_reader_feed(&s->rd, tmp, (size_t)n);
    }
    if (s->rd.overflow) {
        acp_srv_log("client sent a message over the 8 MiB cap; closing");
        s->eof = true;
        return -1;
    }
    for (;;) {
        size_t len = 0;
        char *line = acp_reader_next(&s->rd, &len);
        if (!line) break;
        if (!len) {
            free(line);
            continue;
        }
        yyjson_doc *doc = jparse(line, len);
        free(line);
        if (!doc) {
            acp_send_error(s->out_fd, NULL, ACP_E_PARSE, "invalid JSON");
            continue;
        }
        yyjson_val *root = yyjson_doc_get_root(doc);
        if (root && yyjson_is_arr(root)) { /* tolerate v2 batches on read */
            size_t i, max;
            yyjson_val *el;
            yyjson_arr_foreach(root, i, max, el) handle_message(s, el);
        } else {
            handle_message(s, root);
        }
        yyjson_doc_free(doc);
    }
    return 0;
}

/* ---------- entry point ---------- */

int cmd_acp_server(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    (void)g;
    ctx->mcp_disabled = true; /* the ACP client owns MCP in server mode */
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            const char *path = argv[++i];
            if (!freopen(path, "a", stderr)) {
                /* keep the inherited stderr: stdout must stay protocol-only */
            }
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            free(ctx->model);
            ctx->model = xstrdup(argv[++i]);
        } else {
            fprintf(stderr, "tny: acp: unknown flag %s\nExample: tny acp --model gpt-4.1-mini\n",
                    argv[i]);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);

    acp_srv s;
    memset(&s, 0, sizeof s);
    s.ctx = ctx;
    s.in_fd = 0;
    s.out_fd = 1;
    s.next_id = 1;
    acp_reader_init(&s.rd);
    buf_init(&s.last_error);

    acp_srv_log("serving the native loop on stdio (workspace %s, %s)", ctx->cwd,
                ctx->no_save ? "ephemeral" : "persistent");
    while (!s.eof) {
        if (acp_srv_pump(&s, 1000) != 0) break;
    }

    drop_session(&s);
    mcp_shutdown_all();
    acp_reader_free(&s.rd);
    buf_free(&s.last_error);
    return 0;
}
