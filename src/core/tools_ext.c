/* tools_ext.c — memory, read_tool_result, skills, subagent, vision, MCP glue. */
#include "core/tools.h"
#include "core/image.h"
#include "core/skills.h"
#include "core/ssh.h"
#include "mcp/mcp.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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
    if (n > 0) {
        buf[n] = 0;
        return xstrdup(buf);
    }
#endif
    return xstrdup("tny");
}

/* ---- memory: ~/.tny/memories.json, only written when asked ---- */

static char *t_memory(tools_env *env, yyjson_val *args) {
    const char *action = jget_str(args, "action");
    if (!action) return tool_err("missing action (get|set|list)");
    if (env->ctx->no_save && strcmp(action, "set") == 0)
        return tool_err("persistent memory writes are unavailable in ephemeral mode");
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
            yyjson_mut_doc *m = doc ? yyjson_doc_mut_copy(doc, NULL) : yyjson_mut_doc_new(NULL);
            if (!yyjson_mut_doc_get_root(m)) yyjson_mut_doc_set_root(m, yyjson_mut_obj(m));
            yyjson_mut_obj_put(yyjson_mut_doc_get_root(m), yyjson_mut_strcpy(m, key),
                               yyjson_mut_strcpy(m, value));
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
 * Persistent parents use disk-backed child sessions. Ephemeral parents pass
 * the mode through and therefore support only one-shot children. ---- */

char *tools_subagent_command(tools_env *env, const char *id, const char *prompt,
                             const char *stderr_path) {
    char *exe = self_exe();
    buf_t cmd;
    buf_init(&cmd);
    ssh_shell_quote(&cmd, exe);
    free(exe);
    buf_appends(&cmd, " --cwd ");
    ssh_shell_quote(&cmd, env->ctx->cwd);
    /* The child must run the parent's resolved provider. Without this it
     * re-resolves from settings, where a remembered `last_provider` beats
     * environment detection — a host provider left there makes every
     * subagent spawn (or fail to spawn) the wrong backend. The API key is
     * never put on the command line; it travels through the inherited
     * environment or the same settings the parent read. */
    buf_appends(&cmd, " --provider ");
    ssh_shell_quote(&cmd, tny_provider_name(env->ctx));
    if (env->ctx->backend == TNY_BK_OPENAI && env->ctx->base_url && *env->ctx->base_url) {
        buf_appends(&cmd, " --base-url ");
        ssh_shell_quote(&cmd, env->ctx->base_url);
    }
    if (env->ctx->backend == TNY_BK_OPENAI && env->ctx->wire_api)
        buf_appendf(&cmd, " --wire-api %s",
                    tny_wire_is_chat(env->ctx->wire_api) ? "chat" : "responses");
    if (env->ctx->model) {
        buf_appends(&cmd, " --model ");
        ssh_shell_quote(&cmd, env->ctx->model);
    }
    if (env->ctx->reasoning_effort && *env->ctx->reasoning_effort) {
        buf_appends(&cmd, " --effort ");
        ssh_shell_quote(&cmd, env->ctx->reasoning_effort);
    }
    /* children cannot raise permission mode above the creator */
    buf_appendf(&cmd, " --permission-mode %s", tny_perm_mode_name(env->ctx->perm_mode));
    if (env->ctx->no_save) buf_appends(&cmd, " --ephemeral");
    buf_appends(&cmd, " ask --json");
    if (id) {
        buf_appends(&cmd, " --resume-id ");
        ssh_shell_quote(&cmd, id);
    }
    buf_appends(&cmd, " -- ");
    ssh_shell_quote(&cmd, prompt);
    if (stderr_path) {
        buf_appends(&cmd, " 2>");
        ssh_shell_quote(&cmd, stderr_path);
    }
    return buf_detach(&cmd);
}

/* Slurp the child's captured stderr for a diagnosis; trimmed and bounded so
 * a chatty child cannot flood the tool result. malloc'd, may be NULL. */
static char *subagent_stderr_tail(const char *path) {
    size_t len = 0;
    char *text = path ? file_slurp(path, &len) : NULL;
    if (!text) return NULL;
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) text[--len] = 0;
    if (!len) {
        free(text);
        return NULL;
    }
    if (len > 700) memmove(text, text + len - 700, 701); /* keep the tail: the error is last */
    return text;
}

static char *t_subagent(tools_env *env, yyjson_val *args) {
    const char *action = jget_str(args, "action");
    if (!action) return tool_err("missing action (create|message|inspect|lifecycle)");
    if (!env->ctx->api_key && env->ctx->backend == TNY_BK_OPENAI)
        return tool_err("subagents need the native provider configured");
    if (env->ctx->no_save && (strcmp(action, "message") == 0 || strcmp(action, "inspect") == 0))
        return tool_err("ephemeral subagents are one-shot and cannot be resumed or inspected");

    char *result = NULL;

    if (strcmp(action, "create") == 0 || strcmp(action, "message") == 0) {
        const char *prompt = jget_str(args, "prompt");
        const char *id = jget_str(args, "id");
        if (!prompt) return tool_err("missing prompt");
        if (strcmp(action, "message") == 0 && !id) return tool_err("message needs the subagent id");
        const char *tmpdir = getenv("TMPDIR");
        char errpath[4096];
        snprintf(errpath, sizeof errpath, "%s/tny-subagent-XXXXXX",
                 tmpdir && *tmpdir ? tmpdir : "/tmp");
        int efd = mkstemp(errpath);
        if (efd >= 0) close(efd);
        char *cmd = tools_subagent_command(env, id, prompt, efd >= 0 ? errpath : NULL);
        FILE *f = cmd ? popen(cmd, "r") : NULL;
        free(cmd);
        if (!f) result = tool_err("could not start subagent");
        else {
            buf_t out;
            buf_init(&out);
            char tmp[4096];
            size_t n;
            while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_append(&out, tmp, n);
            int wstatus = pclose(f);
            yyjson_doc *doc = jparse(out.data, out.len);
            if (doc) {
                yyjson_val *root = yyjson_doc_get_root(doc);
                const char *o = jget_str(root, "output");
                const char *sid = jget_str(root, "session_id");
                const char *cerr = jget_str(root, "error");
                int child_exit = (int)jget_int(root, "exit_code", 0);
                if (child_exit != 0 || (cerr && *cerr)) {
                    result = tool_err("subagent failed (exit %d): %s%s%s", child_exit,
                                      cerr && *cerr ? cerr : "no error detail",
                                      o && *o ? "\npartial output:\n" : "", o && *o ? o : "");
                } else {
                    bool resumable = sid && *sid && !env->ctx->no_save;
                    buf_t r;
                    buf_init(&r);
                    if (resumable) {
                        buf_appendf(&r, "subagent %s finished.\n", sid);
                        buf_appendf(&r, "id: %s (use action=message id=%s to continue)\n", sid,
                                    sid);
                    } else {
                        buf_appends(&r, "ephemeral subagent finished; no resumable id was "
                                        "stored.\n");
                    }
                    buf_appendf(&r, "result:\n%s", o ? o : "(no output)");
                    result = tool_bound_result(env, r.data, r.len);
                    buf_free(&r);
                }
                yyjson_doc_free(doc);
            } else {
                /* no JSON on stdout: the child never reached a turn — the
                 * reason (bad provider, missing key, no session) is on its
                 * captured stderr, so surface that instead of nothing */
                char *etail = subagent_stderr_tail(efd >= 0 ? errpath : NULL);
                int ec = wstatus > 0 && WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
                result = tool_err("subagent failed to start (exit %d): %.300s%s%s", ec,
                                  out.data ? out.data : "", etail ? "\n" : "", etail ? etail : "");
                free(etail);
            }
            buf_free(&out);
        }
        if (efd >= 0) unlink(errpath);
        return result;
    }

    if (strcmp(action, "inspect") == 0) {
        const char *id = jget_str(args, "id");
        if (!id) result = tool_err("inspect needs id");
        else {
            tny_session_state *child = session_open(env->ctx, id);
            if (!child) result = tool_err("no session %s", id);
            else {
                buf_t r;
                buf_init(&r);
                buf_appendf(&r, "subagent %s: %d turns, title: %s", id, session_turns(child),
                            session_title(child) ? session_title(child) : "(none)");
                session_close(child);
                result = buf_detach(&r);
            }
        }
    } else if (strcmp(action, "lifecycle") == 0) {
        result = xstrdup(
            env->ctx->no_save
                ? "ephemeral subagents are one-shot processes; child sessions are not stored"
                : "subagents are one-shot processes; sessions persist under tny sessions");
    } else {
        result = tool_err("unknown action %s", action);
    }
    return result;
}

static char *t_read_image(tools_env *env, yyjson_val *args) {
    const char *abs = NULL;
    size_t len = 0;
    const char *mime = NULL;
    char loaderr[256];
    if (tools_queue_image(env, jget_str(args, "path"), false, &abs, &mime, &len, loaderr,
                          sizeof loaderr) != 0)
        return tool_err("%s", loaderr);
    buf_t b;
    buf_init(&b);
    buf_appendf(&b,
                "Image loaded: %s (%s, %zu bytes). The pixels follow in the "
                "next user message — describe what you see; do not call "
                "read_file on this path.",
                abs, mime, len);
    return buf_detach(&b);
}

static char *t_ask_user(tools_env *env, yyjson_val *args) {
    const char *q = jget_str(args, "question");
    if (!q) return tool_err("missing question");
    if (!env->ask_user)
        return tool_err("not interactive; the user cannot be asked right now — "
                        "proceed with your best judgment and note the assumption");
    char *answer = env->ask_user(q, env->ask_user_ud);
    if (answer) return answer;
    return tool_err("not interactive; the user cannot be asked right now — "
                    "proceed with your best judgment and note the assumption");
}

char *tool_ext_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled) {
    *handled = true;
    if (env->ctx->library_mode &&
        (strcmp(name, "subagent") == 0 || strcmp(name, "skill") == 0 ||
         strcmp(name, "install_skill") == 0 || strcmp(name, "memory") == 0 ||
         strcmp(name, "ask_user_question") == 0))
        return tool_err("%s is disabled for embedded runtimes", name);
    if (strcmp(name, "memory") == 0) return t_memory(env, args);
    if (strcmp(name, "read_tool_result") == 0) return t_read_tool_result(env, args);
    if (strcmp(name, "skill") == 0) return t_skill(env, args);
    if (strcmp(name, "install_skill") == 0) return t_install_skill(env, args);
    if (strcmp(name, "subagent") == 0) return t_subagent(env, args);
    if (strcmp(name, "ask_user_question") == 0) return t_ask_user(env, args);
    if (strcmp(name, "read_image") == 0) return t_read_image(env, args);
    if (str_starts(name, "mcp_") && env->ctx->mcp_disabled)
        return tool_err("MCP is disabled for this runtime");
    if (strcmp(name, "mcp_features") == 0) return mcp_features(env);
    if (strcmp(name, "mcp_search_tools") == 0)
        return mcp_search_tools(env, jget_str(args, "query"));
    if (strcmp(name, "mcp_select_tool") == 0) {
        yyjson_val *a = jget(args, "arguments");
        char *aj = a ? jwrite_val(a) : NULL;
        char *res =
            mcp_call_tool(env, jget_str(args, "server"), jget_str(args, "tool"), aj ? aj : "{}");
        free(aj);
        return res;
    }
    *handled = false;
    return NULL;
}
