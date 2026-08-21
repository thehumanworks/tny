/* tools_ext.c — memory, read_tool_result, skills, subagent, vision, MCP glue. */
#include "core/tools.h"
#include "core/skills.h"
#include "mcp/mcp.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static char *self_exe(void) {
#ifdef __APPLE__
    char buf[4096];
    uint32_t sz = sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) == 0) return xstrdup(buf);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) { buf[n] = 0; return xstrdup(buf); }
#endif
    return xstrdup("tny");
}

/* ---- memory: ~/.tny/memories.json, only written when asked ---- */

static char *t_memory(tools_env *env, yyjson_val *args) {
    const char *action = jget_str(args, "action");
    if (!action) return tool_err("missing action (get|set|list)");
    char *file = path_join(env->ctx->tny_dir, "memories.json");
    yyjson_doc *doc = jparse_file(file);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    char *result = NULL;

    if (strcmp(action, "get") == 0) {
        const char *key = jget_str(args, "key");
        if (!key) result = tool_err("missing key");
        else {
            const char *v = jget_str(root, key);
            result = v ? xstrdup(v) : tool_err("no memory named %s", key);
        }
    } else if (strcmp(action, "list") == 0) {
        buf_t out;
        buf_init(&out);
        if (root && yyjson_is_obj(root)) {
            size_t idx, max;
            yyjson_val *k, *v;
            yyjson_obj_foreach(root, idx, max, k, v)
                buf_appendf(&out, "%s: %.120s\n", yyjson_get_str(k),
                            yyjson_is_str(v) ? yyjson_get_str(v) : "…");
        }
        if (!out.len) buf_appends(&out, "(no memories)");
        result = buf_detach(&out);
    } else if (strcmp(action, "set") == 0) {
        const char *key = jget_str(args, "key");
        const char *value = jget_str(args, "value");
        if (!key || !value) result = tool_err("set needs key and value");
        else {
            yyjson_mut_doc *m = doc ? yyjson_doc_mut_copy(doc, NULL)
                                    : yyjson_mut_doc_new(NULL);
            if (!yyjson_mut_doc_get_root(m))
                yyjson_mut_doc_set_root(m, yyjson_mut_obj(m));
            yyjson_mut_obj_put(yyjson_mut_doc_get_root(m),
                               yyjson_mut_strcpy(m, key), yyjson_mut_strcpy(m, value));
            char *out = jwrite_pretty(m);
            yyjson_mut_doc_free(m);
            if (out) {
                mkdir_p(env->ctx->tny_dir);
                file_write_atomic(file, out, strlen(out));
                free(out);
                buf_t b;
                buf_init(&b);
                buf_appendf(&b, "remembered %s", key);
                result = buf_detach(&b);
            } else result = tool_err("could not write memories");
        }
    } else {
        result = tool_err("unknown action %s", action);
    }
    yyjson_doc_free(doc);
    free(file);
    return result;
}

static char *t_read_tool_result(tools_env *env, yyjson_val *args) {
    if (!env->session) return tool_err("no session");
    const char *handle = jget_str(args, "handle");
    if (!handle) return tool_err("missing handle");
    int64_t off = jget_int(args, "offset", 0);
    int64_t len = jget_int(args, "length", 16384);
    if (len <= 0 || len > 65536) len = 16384;
    size_t got = 0;
    char *data = session_read_result(env->session, handle, (size_t)off, (size_t)len, &got);
    if (!data) return tool_err("unknown handle %s", handle);
    return data;
}

static char *t_skill(tools_env *env, yyjson_val *args) {
    const char *name = jget_str(args, "name");
    if (!name) return tool_err("missing name");
    char *body = skills_load(env->ctx, name);
    if (!body) return tool_err("no skill named %s", name);
    char *res = tool_bound_result(env, body, strlen(body));
    free(body);
    return res;
}

static char *t_install_skill(tools_env *env, yyjson_val *args) {
    char *err = NULL;
    char *abs = tool_resolve_path(env, jget_str(args, "path"), &err);
    if (!abs) return err;
    char msg[256];
    if (skills_install(env->ctx, abs, msg, sizeof msg) != 0) {
        free(abs);
        return tool_err("%s", msg);
    }
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "installed skill from %s into ~/.tny/skills", abs);
    free(abs);
    return buf_detach(&b);
}

/* ---- subagent: child native sessions via `tny ask` processes.
 * Disk-backed like everything else; the child transcript stays out of the
 * parent context. ---- */

static char *t_subagent(tools_env *env, yyjson_val *args) {
    const char *action = jget_str(args, "action");
    if (!action) return tool_err("missing action (create|message|inspect|lifecycle)");
    if (!env->ctx->api_key && env->ctx->backend == TNY_BK_OPENAI)
        return tool_err("subagents need the native provider configured");

    char *exe = self_exe();
    buf_t cmd;
    buf_init(&cmd);
    char *result = NULL;

    if (strcmp(action, "create") == 0 || strcmp(action, "message") == 0) {
        const char *prompt = jget_str(args, "prompt");
        const char *id = jget_str(args, "id");
        if (!prompt) { free(exe); buf_free(&cmd); return tool_err("missing prompt"); }
        if (strcmp(action, "message") == 0 && !id) {
            free(exe); buf_free(&cmd);
            return tool_err("message needs the subagent id");
        }
        buf_appendf(&cmd, "'%s' --cwd '%s' ", exe, env->ctx->cwd);
        if (env->ctx->model) buf_appendf(&cmd, "--model '%s' ", env->ctx->model);
        /* children cannot raise permission mode above the creator */
        buf_appendf(&cmd, "--permission-mode %s ", tny_perm_mode_name(env->ctx->perm_mode));
        buf_appends(&cmd, "ask --json ");
        if (id) buf_appendf(&cmd, "--resume-id %s ", id);
        buf_appends(&cmd, "-- ");
        buf_appends(&cmd, "\"");
        for (const char *p = prompt; *p; p++) {
            if (*p == '"' || *p == '\\' || *p == '$' || *p == '`') buf_appends(&cmd, "\\");
            buf_append(&cmd, p, 1);
        }
        buf_appends(&cmd, "\"");
        FILE *f = popen(cmd.data, "r");
        if (!f) result = tool_err("could not start subagent");
        else {
            buf_t out;
            buf_init(&out);
            char tmp[4096];
            size_t n;
            while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_append(&out, tmp, n);
            pclose(f);
            yyjson_doc *doc = jparse(out.data, out.len);
            if (doc) {
                const char *o = jget_str(yyjson_doc_get_root(doc), "output");
                const char *sid = jget_str(yyjson_doc_get_root(doc), "session_id");
                buf_t r;
                buf_init(&r);
                buf_appendf(&r, "subagent %s finished.\n", sid ? sid : "?");
                if (sid) buf_appendf(&r, "id: %s (use action=message id=%s to continue)\n", sid, sid);
                buf_appendf(&r, "result:\n%s", o ? o : "(no output)");
                yyjson_doc_free(doc);
                result = tool_bound_result(env, r.data, r.len);
                buf_free(&r);
            } else {
                result = tool_err("subagent failed: %.300s", out.data ? out.data : "");
            }
            buf_free(&out);
        }
    } else if (strcmp(action, "inspect") == 0) {
        const char *id = jget_str(args, "id");
        if (!id) result = tool_err("inspect needs id");
        else {
            tny_session *child = session_open(env->ctx, id);
            if (!child) result = tool_err("no session %s", id);
            else {
                buf_t r;
                buf_init(&r);
                buf_appendf(&r, "subagent %s: %d turns, title: %s", id,
                            session_turns(child),
                            session_title(child) ? session_title(child) : "(none)");
                session_close(child);
                result = buf_detach(&r);
            }
        }
    } else if (strcmp(action, "lifecycle") == 0) {
        result = xstrdup("subagents are one-shot processes; sessions persist under tny sessions");
    } else {
        result = tool_err("unknown action %s", action);
    }
    free(exe);
    buf_free(&cmd);
    return result;
}

static char *t_ask_user(tools_env *env, yyjson_val *args) {
    const char *q = jget_str(args, "question");
    if (!q) return tool_err("missing question");
    if (!env->prompt)
        return tool_err("not interactive; the user cannot be asked right now — "
                        "proceed with your best judgment and note the assumption");
    /* reuse the permission prompt hook as a yes/no clarifier */
    tny_perm_decision d = env->prompt("ask_user_question", q, env->prompt_ud);
    return xstrdup(d == TNY_PERM_DECISION_DENY ? "user answered: no" : "user answered: yes");
}

char *tool_ext_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled) {
    *handled = true;
    if (strcmp(name, "memory") == 0) return t_memory(env, args);
    if (strcmp(name, "read_tool_result") == 0) return t_read_tool_result(env, args);
    if (strcmp(name, "skill") == 0) return t_skill(env, args);
    if (strcmp(name, "install_skill") == 0) return t_install_skill(env, args);
    if (strcmp(name, "subagent") == 0) return t_subagent(env, args);
    if (strcmp(name, "ask_user_question") == 0) return t_ask_user(env, args);
    if (strcmp(name, "vision") == 0)
        return tool_err("vision runs as a fallback on image attachments; attach "
                        "images with `tny ask --image PATH` or /image in the TUI");
    if (strcmp(name, "mcp_features") == 0) return mcp_features(env);
    if (strcmp(name, "mcp_search_tools") == 0)
        return mcp_search_tools(env, jget_str(args, "query"));
    if (strcmp(name, "mcp_select_tool") == 0) {
        yyjson_val *a = jget(args, "arguments");
        char *aj = a ? jwrite_val(a) : NULL;
        char *res = mcp_call_tool(env, jget_str(args, "server"),
                                  jget_str(args, "tool"), aj ? aj : "{}");
        free(aj);
        return res;
    }
    *handled = false;
    return NULL;
}
