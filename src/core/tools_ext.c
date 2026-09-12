/* tools_ext.c — memory, read_tool_result, skills, vision, MCP glue; the
 * subagent tool lives in subagent.c. */
#include "core/tools.h"
#include "core/speech.h"
#include "core/tools_image.h"
#include "core/image.h"
#include "core/skills.h"
#include "core/subagent.h"
#include "mcp/mcp.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static char *t_speak(tools_env *env, yyjson_val *args) {
    if (env->ctx->library_mode) return tool_err("speak is unavailable in embedded runtimes");
    size_t text_len = 0, voice_len = 0;
    const char *text = jget_strn(args, "text", &text_len);
    const char *voice = jget_strn(args, "voice", &voice_len);
    if (!text || !utf8_valid_bytes(text, text_len) ||
        (voice && !utf8_valid_bytes(voice, voice_len)))
        return tool_err("speech requires UTF-8 text and voice without embedded NUL");
    tny_speech_request r = {
        .text = text, .voice = voice, .cancelled = env->cancelled, .userdata = env->cancelled_ud};
    char err[256] = "";
    int rc = tny_speech_run(env->ctx, &r, err, sizeof err);
    return rc ? tool_err("%s", err) : xstrdup("Speech played successfully.");
}

char *tool_ext_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled) {
    *handled = true;
    /* subagent answers every unsupported runtime itself (stable codes) */
    if (strcmp(name, "subagent") == 0) return tny_subagent_execute(env, args);
    if (env->ctx->library_mode &&
        (strcmp(name, "skill") == 0 || strcmp(name, "install_skill") == 0 ||
         strcmp(name, "memory") == 0 || strcmp(name, "ask_user_question") == 0))
        return tool_err("%s is disabled for embedded runtimes", name);
    /* Image calls carry the plan their permission decision was made about, so
     * they are dispatched by tools_call_execute, never from this name-only
     * chain. Reaching here means no plan was prepared: refuse (ADR 0095). */
    if (strcmp(name, "image_generate") == 0 || strcmp(name, "image_edit") == 0)
        return tool_err("this image call was not prepared; request it again");
    if (strcmp(name, "speak") == 0) return t_speak(env, args);
    if (strcmp(name, "memory") == 0) return t_memory(env, args);
    if (strcmp(name, "read_tool_result") == 0) return t_read_tool_result(env, args);
    if (strcmp(name, "skill") == 0) return t_skill(env, args);
    if (strcmp(name, "install_skill") == 0) return t_install_skill(env, args);
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
