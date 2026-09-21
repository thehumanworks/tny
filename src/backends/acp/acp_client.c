/* acp_client.c — `--backend acp`: spawn an ACP agent and run JSON-RPC 2.0 over
 * its stdio (transport, lifecycle, backend vtable). Message normalization
 * lives in acp_events.c. Contract: docs/backends/acp.md — protocolVersion 1,
 * session/prompt stays pending for the whole turn. */
#include "backends/acp/acp_client.h"
#include "util/util.h"
#include "core/image.h"
#include "core/instructions.h"
#include "core/skills.h"
#include "core/tasks.h"
#include "core/swarm.h"
#include "util/alloc.h"
#include "util/process.h"

#include <errno.h>
#include <limits.h>
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

/* schema-v1.20.0 permits either a flat SessionConfigSelectOption[] or a
 * SessionConfigSelectGroup[] whose entries carry their own options array. */
static bool ac_config_has_value(yyjson_val *options, const char *wanted) {
    if (!options || !yyjson_is_arr(options)) return false;
    size_t idx, max;
    yyjson_val *entry;
    yyjson_arr_foreach(options, idx, max, entry) {
        const char *value = jget_str(entry, "value");
        if (value && strcmp(value, wanted) == 0) return true;
        yyjson_val *group = jget(entry, "options");
        size_t gi, gn;
        yyjson_val *option;
        yyjson_arr_foreach(group, gi, gn, option) {
            value = jget_str(option, "value");
            if (value && strcmp(value, wanted) == 0) return true;
        }
    }
    return false;
}

static int ac_set_legacy_model(ac_impl *o, yyjson_val *models, const char *sid, char *e,
                               size_t el) {
    const char *wanted = o->ctx->model;
    bool found = false;
    size_t i, n;
    yyjson_val *model;
    yyjson_arr_foreach(jget(models, "availableModels"), i, n, model) {
        const char *id = jget_str(model, "modelId");
        if (id && strcmp(id, wanted) == 0) found = true;
    }
    if (!found) {
        snprintf(e, el, "acp: requested model '%.*s' is not advertised by the agent", 120, wanted);
        return -1;
    }
    buf_t p;
    buf_init(&p);
    buf_appends(&p, "{\"sessionId\":");
    jescape(&p, sid);
    buf_appends(&p, ",\"modelId\":");
    jescape(&p, wanted);
    buf_appends(&p, "}");
    yyjson_doc *doc = ac_rpc(o, "session/set_model", p.data, e, el);
    buf_free(&p);
    if (!doc) return -1;
    bool ok = yyjson_is_obj(jget(yyjson_doc_get_root(doc), "result"));
    yyjson_doc_free(doc);
    if (!ok) snprintf(e, el, "acp: malformed session/set_model acknowledgement");
    return ok ? 0 : -1;
}

static yyjson_val *ac_find_model_config(yyjson_val *configs) {
    if (!configs || !yyjson_is_arr(configs)) return NULL;
    size_t idx, max;
    yyjson_val *config;

    /* The semantic category is authoritative when present. */
    yyjson_arr_foreach(configs, idx, max, config) {
        const char *type = jget_str(config, "type");
        const char *category = jget_str(config, "category");
        if (type && category && strcmp(type, "select") == 0 && strcmp(category, "model") == 0)
            return config;
    }
    /* Older agents commonly omit category and use the conventional id. */
    yyjson_arr_foreach(configs, idx, max, config) {
        const char *type = jget_str(config, "type");
        const char *id = jget_str(config, "id");
        if (type && id && strcmp(type, "select") == 0 && strcmp(id, "model") == 0) return config;
    }
    return NULL;
}

static void ac_append_model(buf_t *out, const char *id, const char *name) {
    if (!id || !*id) return;
    if (out->len > 1) buf_appends(out, ",");
    buf_appends(out, "{\"id\":");
    jescape(out, id);
    buf_appends(out, ",\"name\":");
    jescape(out, name ? name : id);
    buf_appends(out, "}");
}

static void ac_remember_models(ac_impl *o, yyjson_val *result) {
    buf_t out;
    buf_init(&out);
    buf_appends(&out, "[");
    yyjson_val *config = ac_find_model_config(jget(result, "configOptions"));
    size_t i, n;
    yyjson_val *item;
    if (config) {
        yyjson_arr_foreach(jget(config, "options"), i, n, item) {
            ac_append_model(&out, jget_str(item, "value"), jget_str(item, "name"));
            size_t gi, gn;
            yyjson_val *entry;
            yyjson_arr_foreach(jget(item, "options"), gi, gn, entry)
                ac_append_model(&out, jget_str(entry, "value"), jget_str(entry, "name"));
        }
    } else {
        yyjson_arr_foreach(jget(jget(result, "models"), "availableModels"), i, n, item)
            ac_append_model(&out, jget_str(item, "modelId"), jget_str(item, "name"));
    }
    buf_appends(&out, "]");
    free(o->models_json);
    o->models_json = buf_detach(&out);
}

static yyjson_val *ac_find_config(yyjson_val *configs, const char *category) {
    size_t i, n;
    yyjson_val *entry;
    yyjson_arr_foreach(configs, i, n, entry) {
        const char *cat = jget_str(entry, "category");
        const char *type = jget_str(entry, "type");
        if (cat && type && strcmp(type, "select") == 0 && strcmp(cat, category) == 0) return entry;
    }
    return NULL;
}

static int ac_set_config(ac_impl *o, yyjson_val *config, const char *sid, const char *wanted,
                         const char *label, char *e, size_t el) {
    const char *config_id = jget_str(config, "id");
    if (!config_id || !ac_config_has_value(jget(config, "options"), wanted)) {
        snprintf(e, el, "acp: requested %s '%.*s' is not advertised by the agent", label, 120,
                 wanted);
        return -1;
    }
    buf_t p;
    buf_init(&p);
    buf_appends(&p, "{\"sessionId\":");
    jescape(&p, sid);
    buf_appends(&p, ",\"configId\":");
    jescape(&p, config_id);
    buf_appends(&p, ",\"value\":");
    jescape(&p, wanted);
    buf_appends(&p, "}");
    yyjson_doc *doc = ac_rpc(o, "session/set_config_option", p.data, e, el);
    buf_free(&p);
    if (!doc) return -1;
    yyjson_val *confirmed = NULL;
    yyjson_val *configs = jget(jget(yyjson_doc_get_root(doc), "result"), "configOptions");
    size_t idx, max;
    yyjson_val *candidate;
    yyjson_arr_foreach(configs, idx, max, candidate) {
        const char *id = jget_str(candidate, "id");
        if (id && strcmp(id, config_id) == 0) {
            confirmed = candidate;
            break;
        }
    }
    const char *current = jget_str(confirmed, "currentValue");
    if (!current || strcmp(current, wanted) != 0) {
        snprintf(e, el, "acp: agent did not confirm requested %s '%.*s' after configuration", label,
                 120, wanted);
        yyjson_doc_free(doc);
        return -1;
    }
    if (strcmp(label, "model") != 0 && o->ctx->model && o->config_doc &&
        ac_find_model_config(
            jget(jget(yyjson_doc_get_root(o->config_doc), "result"), "configOptions"))) {
        const char *model = jget_str(ac_find_model_config(configs), "currentValue");
        if (!model || strcmp(model, o->ctx->model) != 0) {
            snprintf(e, el, "acp: agent changed the requested model while configuring %s", label);
            yyjson_doc_free(doc);
            return -1;
        }
    }
    yyjson_doc_free(o->config_doc);
    o->config_doc = doc;
    return 0;
}

static int ac_set_requested_model(ac_impl *o, yyjson_val *result, const char *sid, char *e,
                                  size_t el) {
    const char *wanted = o->ctx->model;
    if (!wanted || !*wanted) return 0;
    yyjson_val *config = ac_find_model_config(jget(result, "configOptions"));
    if (!config && jget(result, "models"))
        return ac_set_legacy_model(o, jget(result, "models"), sid, e, el);
    if (!config) {
        snprintf(e, el, "acp: agent did not advertise a selectable model option");
        return -1;
    }
    return ac_set_config(o, config, sid, wanted, "model", e, el);
}

static int ac_set_session_options(ac_impl *o, yyjson_val *result, const char *sid, char *e,
                                  size_t el) {
    if (o->config_doc) result = jget(yyjson_doc_get_root(o->config_doc), "result");
    if (o->ctx->reasoning_effort && *o->ctx->reasoning_effort) {
        yyjson_val *config = ac_find_config(jget(result, "configOptions"), "thought_level");
        if (!config) {
            snprintf(e, el, "acp: agent does not advertise thought_level; omit --effort");
            return -1;
        }
        if (ac_set_config(o, config, sid, o->ctx->reasoning_effort, "reasoning effort", e, el) != 0)
            return -1;
        result = jget(yyjson_doc_get_root(o->config_doc), "result");
    }
    if (o->claude_agent) {
        yyjson_val *config = ac_find_config(jget(result, "configOptions"), "mode");
        /* Only the pinned tools-only adapter may delegate all authority to tny.
         * Its native and account MCP tools are disabled in session metadata. */
        const char *mode = o->claude_verified ? "bypassPermissions" : "default";
        if (ac_set_config(o, config, sid, mode, "permission mode", e, el) != 0) return -1;
    }
    return 0;
}

const char *ac_agent_cwd(const ac_impl *o) {
    return o->ctx->ssh_host || o->ctx->workspace_read_only || o->ctx->acp_require_tools_authority
               ? tny_acp_bridge_directory(o->bridge)
               : o->ctx->cwd;
}

/* ---------- vtable ---------- */

bool ac_guarded(const ac_impl *o) {
    return o->ctx->acp_require_tools_authority || o->ctx->workspace_read_only;
}

static bool ac_cleanup_receipt(ac_impl *o) {
    if (!o->cleanup_receipt_pending || !o->ctx->acp_cleanup_file) return true;
    if (tny_alloc_settling() || tny_alloc_scope_failed()) return false;
    char temp[PATH_MAX];
    int n = snprintf(temp, sizeof temp, "%s.XXXXXX", o->ctx->acp_cleanup_file);
    if (n < 0 || (size_t)n >= sizeof temp) return false;
    int fd = mkstemp(temp); /* fresh owner-only inode, never a preexisting symlink */
    if (fd < 0) return false;
    const char receipt[] = "complete\n";
    size_t offset = 0;
    while (offset < sizeof receipt - 1) {
        ssize_t written = write(fd, receipt + offset, sizeof receipt - 1 - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        offset += (size_t)written;
    }
    bool ok = close(fd) == 0 && offset == sizeof receipt - 1;
    if (ok) ok = rename(temp, o->ctx->acp_cleanup_file) == 0;
    if (!ok) unlink(temp);
    if (ok) o->cleanup_receipt_pending = false;
    return ok;
}

/* Managed completion is not publishable until the still-owned adapter tree
 * has been captured and stopped. Do not close stdin or reap the root first:
 * either action can let its descendants escape ancestry-based accounting. */
bool ac_stop_owned_agent(ac_impl *o) {
    if (!ac_guarded(o)) return true;
    if ((o->cleanup_receipt_pending || o->cleanup_unknown) && o->ctx->acp_cleanup_file &&
        unlink(o->ctx->acp_cleanup_file) != 0 && errno != ENOENT)
        o->cleanup_unknown = true;
    if (o->cleanup_unknown) return false;
    if (o->pid <= 0) {
        if (!ac_cleanup_receipt(o)) o->cleanup_unknown = true;
        return !o->cleanup_unknown;
    }
    if (tny_alloc_settling() || tny_alloc_scope_failed()) {
        o->cleanup_unknown = true;
        return false; /* resource-only disconnect owns the OOM fallback */
    }
    int status = 0;
    bool reaped = false;
    int rc = tny_process_stop_owned_tree(o->pid, &status, &reaped);
    if (reaped) o->pid = o->pgid = 0;
    if (rc == 0 && !ac_cleanup_receipt(o)) rc = -1;
    if (rc != 0) o->cleanup_unknown = true;
    if (o->in_fd >= 0) close(o->in_fd);
    if (o->out_fd >= 0) close(o->out_fd);
    if (o->err_fd >= 0) close(o->err_fd);
    o->in_fd = o->out_fd = o->err_fd = -1;
    return rc == 0;
}

static int ac_setup_failed(ac_impl *o, char *errbuf, size_t errlen) {
    if (!ac_stop_owned_agent(o)) snprintf(errbuf, errlen, "%s", ACP_CLEANUP_UNKNOWN);
    return -1;
}

static void ac_disconnect(tny_backend *b) {
    ac_impl *o = b->impl;
    (void)ac_stop_owned_agent(o);
    if (o->bridge) {
        if (tny_alloc_settling() || tny_alloc_scope_failed()) tny_acp_bridge_abort(o->bridge);
        else tny_acp_bridge_end_turn(o->bridge);
    }
    if (o->pgid > 0) {
        pid_t pgid = o->pgid; /* the spawn made the agent its group leader */
        if (o->in_fd >= 0) close(o->in_fd);
        o->in_fd = -1;
        int status = 0;
        if (kill(-pgid, SIGTERM) != 0 && o->pid > 0) kill(o->pid, SIGTERM);
        int64_t deadline = monotonic_ms() + 500;
        while (o->pid > 0) {
            pid_t r = waitpid(o->pid, &status, WNOHANG);
            if (r == o->pid || (r < 0 && errno == ECHILD)) {
                o->pid = 0;
                break;
            }
            int64_t remaining = deadline - monotonic_ms();
            if (remaining <= 0) break;
            /* Closed/readable pipes must not turn the grace period into a spin. */
            tny_poll(NULL, 0, remaining < 10 ? (int)remaining : 10);
        }
        if (kill(-pgid, SIGKILL) != 0 && o->pid > 0) kill(o->pid, SIGKILL);
        if (o->pid > 0) {
            /* Never block before escalation, including agents ignoring TERM. */
            while (waitpid(o->pid, &status, 0) < 0 && errno == EINTR) {}
            o->pid = 0;
        }
        o->pgid = 0;
    }
    if (o->in_fd >= 0) {
        close(o->in_fd);
        o->in_fd = -1;
    }
    if (o->out_fd >= 0) {
        close(o->out_fd);
        o->out_fd = -1;
    }
    if (o->err_fd >= 0) {
        close(o->err_fd);
        o->err_fd = -1;
    }
}

static int ac_connect(tny_backend *b, char *errbuf, size_t errlen) {
    ac_impl *o = b->impl;
    if (o->cleanup_unknown) {
        snprintf(errbuf, errlen, "%s", ACP_CLEANUP_UNKNOWN);
        return -1;
    }
    if (o->pid > 0) return 0;
    if (o->ctx->acp_cleanup_file) {
        o->cleanup_receipt_pending = true;
        if (unlink(o->ctx->acp_cleanup_file) != 0 && errno != ENOENT) {
            o->cleanup_unknown = true;
            snprintf(errbuf, errlen, "%s", ACP_CLEANUP_UNKNOWN);
            return -1;
        }
    }
    if (!ac_platform_supported()) {
        snprintf(
            errbuf, errlen,
            "acp: external agent processes are unsupported on WebAssembly; use a native tny build");
        return ac_setup_failed(o, errbuf, errlen);
    }
    if (o->ctx->output_schema) {
        snprintf(errbuf, errlen,
                 "acp: structured output schemas are unsupported; omit --json-schema");
        return ac_setup_failed(o, errbuf, errlen);
    }
    if (!o->bridge) o->bridge = tny_acp_bridge_new(o->ctx, errbuf, errlen);
    if (!o->bridge) return ac_setup_failed(o, errbuf, errlen);
    acp_reader_free(&o->out_r);
    acp_reader_free(&o->err_r);
    acp_reader_init(&o->out_r);
    acp_reader_init(&o->err_r);
    if (ac_spawn_agent(o, errbuf, errlen) != 0) return ac_setup_failed(o, errbuf, errlen);

    buf_t p;
    buf_init(&p);
    buf_appendf(&p,
                "{\"protocolVersion\":%d,\"clientCapabilities\":{\"fs\":{\"readTextFile\":false,"
                "\"writeTextFile\":false},\"terminal\":false},\"clientInfo\":{\"name\":\"tny\","
                "\"title\":\"tny\",\"version\":\"%s\"}}",
                ACP_PROTOCOL_VERSION, TNY_VERSION);
    yyjson_doc *doc = ac_rpc(o, "initialize", p.data, errbuf, errlen);
    buf_free(&p);
    if (!doc) {
        ac_disconnect(b);
        return ac_setup_failed(o, errbuf, errlen);
    }

    yyjson_val *res = jget(yyjson_doc_get_root(doc), "result");
    int64_t ver = jget_int(res, "protocolVersion", -1);
    o->load_session = jget_bool(jget(res, "agentCapabilities"), "loadSession", false);
    o->image_prompts =
        jget_bool(jget(jget(res, "agentCapabilities"), "promptCapabilities"), "image", false);
    const char *agent_name = jget_str(jget(res, "agentInfo"), "name");
    const char *agent_version = jget_str(jget(res, "agentInfo"), "version");
    o->claude_agent =
        agent_name && strcmp(agent_name, "@agentclientprotocol/claude-agent-acp") == 0;
    o->claude_verified = o->claude_agent && agent_version && strcmp(agent_version, "0.75.1") == 0;
    yyjson_doc_free(doc);
    if (o->ctx->ssh_host && !o->claude_verified) {
        snprintf(errbuf, errlen,
                 "acp: --ssh requires verified Claude ACP 0.75.1 tools-only support; other "
                 "external agents may execute workspace tools locally");
        ac_disconnect(b);
        return ac_setup_failed(o, errbuf, errlen);
    }
    if ((o->ctx->acp_require_tools_authority || o->ctx->workspace_read_only) &&
        !o->claude_verified) {
        snprintf(errbuf, errlen,
                 "acp: managed or read-only work requires verified Claude ACP 0.75.1 "
                 "tools-only support; adapter identity did not match");
        ac_disconnect(b);
        return ac_setup_failed(o, errbuf, errlen);
    }
    if (o->ctx->max_steps > 0 && !o->claude_verified) {
        snprintf(errbuf, errlen,
                 "acp: --max-steps requires verified Claude ACP 0.75.1 maxTurns support; "
                 "this agent cannot enforce the requested limit");
        ac_disconnect(b);
        return ac_setup_failed(o, errbuf, errlen);
    }

    if (ver != ACP_PROTOCOL_VERSION) {
        snprintf(errbuf, errlen, "acp: agent negotiated protocolVersion %lld; tny speaks %d",
                 (long long)ver, ACP_PROTOCOL_VERSION);
        ac_disconnect(b);
        return ac_setup_failed(o, errbuf, errlen);
    }
    return 0;
}

static int ac_create_or_resume(tny_backend *b, const char *ptr, char *e, size_t el) {
    ac_impl *o = b->impl;
    bool resume = ptr && *ptr;
    yyjson_doc_free(o->config_doc);
    o->config_doc = NULL;
    o->usage_seen = o->usage_has_cost = false;
    o->usage_context_used = o->usage_context_size = -1;
    o->usage_currency[0] = '\0';
    if (resume && !o->load_session) {
        snprintf(e, el,
                 "acp: agent does not advertise loadSession; start a new tny session instead");
        return ac_setup_failed(o, e, el);
    }
    const char *cwd = ac_agent_cwd(o);
    if (!cwd || cwd[0] != '/') {
        snprintf(e, el, "acp: session cwd must be absolute");
        return ac_setup_failed(o, e, el);
    }
    buf_t p;
    buf_init(&p);
    buf_appends(&p, "{\"cwd\":");
    jescape(&p, cwd);
    if (resume) {
        buf_appends(&p, ",\"sessionId\":");
        jescape(&p, ptr);
    }
    buf_appends(&p, ",\"mcpServers\":");
    buf_appends(&p, tny_acp_bridge_servers_json(o->bridge));
    if (o->claude_agent) {
        buf_appends(&p, ",\"_meta\":{\"disableBuiltInTools\":true,\"claudeCode\":{\"options\":{"
                        "\"tools\":[],\"settingSources\":[],\"strictMcpConfig\":true");
        if (o->claude_verified && o->ctx->max_steps > 0)
            buf_appendf(&p, ",\"maxTurns\":%d", o->ctx->max_steps);
        buf_appends(&p, "}}}");
    }
    buf_appends(&p, "}");
    yyjson_doc *doc = ac_rpc(o, resume ? "session/load" : "session/new", p.data, e, el);
    buf_free(&p);
    if (!doc) return ac_setup_failed(o, e, el);
    yyjson_val *result = jget(yyjson_doc_get_root(doc), "result");
    const char *sid = resume ? ptr : jget_str(result, "sessionId");
    if (!sid || !*sid || strlen(sid) > 4096) {
        snprintf(e, el, "acp: session/new returned no valid sessionId");
        yyjson_doc_free(doc);
        return ac_setup_failed(o, e, el);
    }
    ac_remember_models(o, result);
    if (ac_set_requested_model(o, result, sid, e, el) != 0 ||
        ac_set_session_options(o, result, sid, e, el) != 0) {
        yyjson_doc_free(doc);
        return ac_setup_failed(o, e, el);
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

static int ac_send(tny_backend *b, const char *prompt, const char **images, tny_backend_event_cb cb,
                   void *ud, char *errbuf, size_t errlen) {
    ac_impl *o = b->impl;
    if (o->pid <= 0 && o->session_id) {
        /* OOM or managed completion retained the id, but no live protocol state. */
        char *pointer = ac_session_pointer(b);
        if (!pointer) {
            snprintf(errbuf, errlen, "acp: out of memory retaining session pointer");
            return -1;
        }
        int rc = ac_connect(b, errbuf, errlen);
        if (rc == 0) rc = ac_create_or_resume(b, pointer, errbuf, errlen);
        free(pointer);
        if (rc != 0) {
            ac_disconnect(b);
            return -1;
        }
    }
    if (!o->session_id) {
        snprintf(errbuf, errlen, "acp: no session (call create_or_resume first)");
        return -1;
    }
    if (images && images[0] && (!o->image_prompts || tny_image_input_refused(o->ctx))) {
        snprintf(errbuf, errlen,
                 "acp: image input is disabled or the agent does not advertise image prompts");
        return -1;
    }
    o->cb = cb;
    o->ud = ud;
    o->cancelled = false;
    o->permission_denied = false;
    ac_perms_clear(o);
    tny_acp_bridge_begin_turn(o->bridge, cb, ud);

    buf_t context;
    buf_init(&context);
    if (tny_acp_bridge_prepare_prompt(o->bridge, &context, errbuf, errlen) != 0) {
        buf_free(&context);
        if (tny_alloc_scope_failed()) tny_acp_bridge_abort(o->bridge);
        else tny_acp_bridge_end_turn(o->bridge);
        return ac_setup_failed(o, errbuf, errlen);
    }
    buf_t p;
    buf_init(&p);
    buf_appends(&p, "{\"sessionId\":");
    jescape(&p, o->session_id);
    buf_appends(&p, ",\"prompt\":[");
    buf_appends(
        &context,
        "tny harness context: use the tny MCP server tools for workspace operations so harness "
        "permissions and events apply. External agent built-in tools have separate semantics.\n");
    if (o->ctx->ssh_host)
        buf_appendf(&context,
                    "Workspace tools run remotely over SSH. Remote cwd: %s. The agent local cwd is "
                    "private staging, never your workspace.\n",
                    o->ctx->ssh_cwd);
    tny_swarm_policy(o->ctx, &context);
    instructions_collect(o->ctx, &context);
    if (o->ctx->task_instructions && *o->ctx->task_instructions) tny_task_collect(o->ctx, &context);
    if (o->ctx->system_prompt && *o->ctx->system_prompt) {
        buf_appends(&context, "\nAdditional instructions:\n");
        buf_appends(&context, o->ctx->system_prompt);
    }
    if (!o->ctx->library_mode) {
        int count = 0;
        skill_meta *skills = skills_discover(o->ctx, &count);
        if (count) buf_appends(&context, "\nAvailable skills (load with the skill tool):\n");
        for (int i = 0; i < count; i++)
            buf_appendf(&context, "- %s: %.140s\n", skills[i].name, skills[i].description);
        skills_free(skills, count);
    }
    acp_append_text_block(&p, context.data ? context.data : "", context.len);
    buf_free(&context);
    buf_appends(&p, ",");
    acp_append_text_block(&p, prompt, strlen(prompt));
    for (int i = 0; images && images[i]; i++) {
        size_t len = 0;
        const char *mime = NULL;
        uint8_t *data = image_load(images[i], &len, &mime, errbuf, errlen);
        if (!data) {
            buf_free(&p);
            tny_acp_bridge_end_turn(o->bridge);
            return -1;
        }
        if (len > ACP_MAX_MSG / 2 || p.len > ACP_MAX_MSG - len * 2) {
            free(data);
            buf_free(&p);
            tny_acp_bridge_end_turn(o->bridge);
            snprintf(errbuf, errlen, "acp: image prompt exceeds the 8 MiB message cap");
            return -1;
        }
        buf_appends(&p, ",{\"type\":\"image\",\"mimeType\":");
        jescape(&p, mime);
        buf_appends(&p, ",\"data\":\"");
        b64_encode(data, len, &p);
        free(data);
        buf_appends(&p, "\"}");
    }
    buf_appends(&p, "]}");

    if (p.len > ACP_MAX_MSG) {
        buf_free(&p);
        tny_acp_bridge_end_turn(o->bridge);
        snprintf(errbuf, errlen, "acp: prompt exceeds the 8 MiB message cap");
        return -1;
    }
    o->prompt_id = o->next_id++;
    o->turn_active = true;
    int rc = ac_tx_request(o, o->prompt_id, "session/prompt", p.data);
    buf_free(&p);
    if (rc != 0) {
        o->turn_active = false;
        tny_acp_bridge_end_turn(o->bridge);
        snprintf(errbuf, errlen, "acp: agent closed its input");
        return -1;
    }
    return 0;
}

static void ac_cancel(tny_backend *b) {
    ac_impl *o = b->impl;
    if ((!o->turn_active || o->cancelled) && !tny_alloc_settling()) return;
    o->cancelled = true;
    o->cancel_deadline = monotonic_ms() + 2000;
    if (tny_alloc_settling()) {
        ac_disconnect(b);
        ac_perms_clear(o);
        acp_reader_free(&o->out_r);
        acp_reader_free(&o->err_r);
        acp_reader_init(&o->out_r);
        acp_reader_init(&o->err_r);
        yyjson_doc_free(o->wait_doc);
        o->wait_doc = NULL;
        o->wait_id = -1;
        o->turn_active = false;
        return;
    }
    if (o->bridge) tny_acp_bridge_cancel(o->bridge);
    buf_t p;
    buf_init(&p);
    buf_appends(&p, "{\"sessionId\":");
    jescape(&p, o->session_id ? o->session_id : "");
    buf_appends(&p, "}");
    ac_tx_notify(o, "session/cancel", p.data);
    buf_free(&p);
    /* Answer everything still pending so the agent can unwind. */
    while (o->nperms) {
        ac_tx_result(o, o->perms[0].id_raw, "{\"outcome\":{\"outcome\":\"cancelled\"}}");
        ac_perm_drop(o, &o->perms[0]);
    }
}

static void ac_respond_permission(tny_backend *b, const char *perm_id, tny_perm_decision d) {
    ac_impl *o = b->impl;
    if (!perm_id) return;
    if (o->bridge && tny_acp_bridge_respond_permission(o->bridge, perm_id, d)) return;
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
    case TNY_PERM_DECISION_ALLOW: choice = p->allow_once ? p->allow_once : p->allow_always; break;
    default:
        choice = p->reject;
        o->permission_denied = true;
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

static int ac_poll_timeout(tny_backend *b) {
    ac_impl *o = b->impl;
    if (o->turn_active && o->cancelled) {
        int64_t left = o->cancel_deadline - monotonic_ms();
        return left > 0 && left < 50 ? (int)left : (left <= 0 ? 0 : 50);
    }
    return 50; /* async bridge completions and bounded stdout drainage */
}

tny_acp_bridge *tny_backend_acp_bridge(tny_backend *b) {
    return b && b->id == TNY_BK_ACP ? ((ac_impl *)b->impl)->bridge : NULL;
}

static int ac_dispatch(tny_backend *b, struct pollfd *fds, int n) {
    (void)fds;
    (void)n;
    ac_impl *o = b->impl;
    if (o->turn_active && o->cancelled && monotonic_ms() >= o->cancel_deadline) {
        ac_emit_end(o, TNY_STOP_INTERRUPTED);
        ac_disconnect(b);
        return 0;
    }
    if (o->out_fd < 0) return 0;
    int rc = ac_pump_reads(o);
    if (o->turn_active && !o->cancelled && o->bridge &&
        (tny_acp_bridge_stop_requested(o->bridge) || tny_acp_bridge_permission_blocked(o->bridge)))
        ac_cancel(b);
    if (rc == -3 || tny_alloc_scope_failed()) {
        tny_alloc_provider_failed();
        return -1;
    }
    if (rc == -4) {
        fail_turn(o, "acp: malformed JSON-RPC message from agent");
        ac_disconnect(b);
        return -1;
    }
    if (rc == -2) {
        fail_turn(o, "acp: agent sent a message over the 8 MiB cap");
        ac_disconnect(b);
        return -1;
    }
    if (rc == -1) {
        /* stdout closed: give the child a moment to land its exit status so
         * the turn error can name it. */
        int code = ac_reap_agent(o);
        if (o->turn_active) {
            char msg[160];
            if (code == 127)
                snprintf(msg, sizeof msg, "acp: agent '%s' could not be executed",
                         o->ctx->agent_argv && o->ctx->agent_argv[0] ? o->ctx->agent_argv[0] : "?");
            else if (code >= 0)
                snprintf(msg, sizeof msg, "acp: agent exited (status %d) mid-turn", code);
            else snprintf(msg, sizeof msg, "acp: agent closed the connection mid-turn");
            fail_turn(o, msg);
        }
        ac_disconnect(b);
        return -1;
    }
    return 0;
}

static int ac_list_models(tny_backend *b, char **out, char *errbuf, size_t errlen) {
    ac_impl *o = b->impl;
    if (!o->models_json && ac_create_or_resume(b, NULL, errbuf, errlen) != 0) return -1;
    *out = xstrdup(o->models_json ? o->models_json : "[]");
    return *out ? 0 : -1;
}

static int ac_doctor(struct tny_ctx *ctx, char *line, size_t linelen) {
    if (!ctx->agent_argv || !ctx->agent_argv[0]) {
        snprintf(line, linelen,
                 "acp: no agent configured (tny --provider acp --agent CMD -- args…)");
        return 1;
    }
    if (!ac_on_path(ctx->agent_argv[0])) {
        snprintf(line, linelen, "acp: agent '%.80s' not found on PATH", ctx->agent_argv[0]);
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
    free(o->models_json);
    yyjson_doc_free(o->config_doc);
    tny_acp_bridge_destroy(o->bridge);
    if (o->wait_doc) yyjson_doc_free(o->wait_doc);
    free(o);
    free(b);
}

char *tny_backend_acp_usage_json(tny_backend *b) {
    if (!b || b->id != TNY_BK_ACP) return NULL;
    ac_impl *o = b->impl;
    if (!o->usage_seen) return NULL;
    buf_t out;
    buf_init(&out);
    buf_appends(&out, "{\"input_tokens\":null,\"output_tokens\":null,\"context_used\":");
    if (o->usage_context_used >= 0) buf_appendf(&out, "%lld", (long long)o->usage_context_used);
    else buf_appends(&out, "null");
    buf_appends(&out, ",\"context_size\":");
    if (o->usage_context_size >= 0) buf_appendf(&out, "%lld", (long long)o->usage_context_size);
    else buf_appends(&out, "null");
    buf_appends(&out, ",\"cost\":");
    if (o->usage_has_cost) buf_appendf(&out, "%.12g", o->usage_cost);
    else buf_appends(&out, "null");
    buf_appends(&out, ",\"cost_currency\":");
    if (o->usage_currency[0]) jescape(&out, o->usage_currency);
    else buf_appends(&out, "null");
    buf_appends(&out, ",\"cost_scope\":\"session\"}");
    return buf_detach(&out);
}

bool tny_backend_acp_available(void) { return ac_platform_supported(); }

tny_backend *tny_backend_acp_new(struct tny_ctx *ctx) {
    tny_backend *b = calloc(1, sizeof *b);
    ac_impl *o = calloc(1, sizeof *o);
    if (!b || !o) {
        free(b);
        free(o);
        return NULL;
    }
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
    b->poll_timeout = ac_poll_timeout;
    b->doctor = ac_doctor;
    b->list_models = ac_list_models;
    b->destroy = ac_destroy;
    return b;
}
