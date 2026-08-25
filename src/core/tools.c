/* tools.c — registry, permission gate, dispatch, result bounding. */
#include "core/tools.h"
#include "core/image.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

char *tool_err(const char *fmt, ...) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "error: ");
    va_list ap;
    va_start(ap, fmt);
    char line[1024];
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    buf_appends(&b, line);
    return buf_detach(&b);
}

char *tool_resolve_path(tools_env *env, const char *path, char **err_out) {
    *err_out = NULL;
    if (!path || !*path) {
        *err_out = tool_err("missing path");
        return NULL;
    }
    char *expanded = NULL;
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        char *home = path_home();
        expanded = path[1] == '/' ? path_join(home, path + 2) : xstrdup(home);
        free(home);
        path = expanded;
    }
    char *abs;
    if (path[0] == '/') abs = path_abs(path);
    else {
        char *joined = path_join(env->ctx->cwd, path);
        abs = path_abs(joined);
        if (!abs) abs = joined; else free(joined);
    }
    free(expanded);
    if (!abs) {
        *err_out = tool_err("cannot resolve path %s", path);
        return NULL;
    }
    return abs;
}

char *tool_bound_result(tools_env *env, const char *data, size_t len) {
    size_t maxb = env->ctx->max_tool_result_bytes;
    if (len <= maxb) return xstrndup(data, len);
    char *handle = env->session ? session_store_result(env->session, data, len) : NULL;
    buf_t b;
    buf_init(&b);
    buf_append(&b, data, maxb / 2);
    buf_appendf(&b,
                "\n…[truncated: %zu of %zu bytes shown]", maxb / 2, len);
    if (handle)
        buf_appendf(&b,
                    "\nFull output stored as handle \"%s\" — read more with "
                    "read_tool_result(handle, offset, length).", handle);
    free(handle);
    return buf_detach(&b);
}

/* ---- schema ----
 * Compact hand-written JSON; one entry per tool. */
static const char *SCHEMA_JSON =
"["
"{\"type\":\"function\",\"function\":{\"name\":\"list_files\",\"description\":\"List directory entries. Args: path (default workspace root).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"glob_files\",\"description\":\"Find files matching a glob pattern (e.g. src/**/*.c) under the workspace.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"grep_files\",\"description\":\"Search file contents for a substring or simple pattern. Returns matching lines with file:line prefixes.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"case_insensitive\":{\"type\":\"boolean\"}},\"required\":[\"pattern\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":\"Read a text file. Args: path, optional offset (line), limit (lines). For png/jpeg/gif/webp use read_image.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"path\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"read_image\",\"description\":\"View an image file (png/jpeg/gif/webp). The pixels are shown to you on the next turn. Args: path.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"write_file\",\"description\":\"Create or overwrite a file with the given content.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"edit_file\",\"description\":\"Replace an exact substring in a file once (or all occurrences with replace_all).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_string\":{\"type\":\"string\"},\"new_string\":{\"type\":\"string\"},\"replace_all\":{\"type\":\"boolean\"}},\"required\":[\"path\",\"old_string\",\"new_string\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"delete_file\",\"description\":\"Delete a file.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"rename_file\",\"description\":\"Rename or move a file.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"new_path\":{\"type\":\"string\"}},\"required\":[\"path\",\"new_path\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"copy_file\",\"description\":\"Copy a file.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"new_path\":{\"type\":\"string\"}},\"required\":[\"path\",\"new_path\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"create_folder\",\"description\":\"Create a directory (with parents).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"file_info\",\"description\":\"Stat a path: size, type, mtime.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"semantic_search\",\"description\":\"Lexical relevance search over workspace files for a natural-language query. Returns the best-matching files and lines.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"open_file\",\"description\":\"Open a file or URL with the OS default handler.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"terminal\",\"description\":\"Run a shell command in the workspace. Args: command, optional timeout_s (default 120), background (returns immediately with a log path).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"timeout_s\":{\"type\":\"integer\"},\"background\":{\"type\":\"boolean\"}},\"required\":[\"command\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"web_fetch\",\"description\":\"HTTP GET a URL and return the (bounded) body text.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"web_search\",\"description\":\"Search the web (requires a configured search provider).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"memory\",\"description\":\"Persist or recall a user-level note. Args: action get|set|list, key, value.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},\"key\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"action\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"read_tool_result\",\"description\":\"Read a byte range of a stored large tool result. Args: handle, offset, length.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"handle\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"length\":{\"type\":\"integer\"}},\"required\":[\"handle\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"skill\",\"description\":\"Load a skill body by name (discovered from SKILL.md files).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"install_skill\",\"description\":\"Install a skill directory into ~/.tny/skills.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"subagent\",\"description\":\"Manage session-backed child agents. Args: action create|message|inspect|lifecycle, id, prompt.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},\"id\":{\"type\":\"string\"},\"prompt\":{\"type\":\"string\"}},\"required\":[\"action\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"mcp_search_tools\",\"description\":\"Search configured MCP servers for tools matching a query.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"mcp_select_tool\",\"description\":\"Select an MCP tool by server and name, then call it with JSON arguments.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"server\":{\"type\":\"string\"},\"tool\":{\"type\":\"string\"},\"arguments\":{\"type\":\"object\"}},\"required\":[\"server\",\"tool\"]}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"mcp_features\",\"description\":\"List configured MCP servers and their advertised capabilities.\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}},"
"{\"type\":\"function\",\"function\":{\"name\":\"ask_user_question\",\"description\":\"Ask the user a clarifying question (interactive sessions only).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\"}},\"required\":[\"question\"]}}}"
"]";

static bool schema_tool_disabled(const tools_env *env, const char *name) {
    if (!env || !env->ctx || !name) return false;
    if (env->ctx->mcp_disabled && str_starts(name, "mcp_")) return true;
    if (!env->ctx->library_mode) return false;
    return strcmp(name, "subagent") == 0 || strcmp(name, "skill") == 0 ||
           strcmp(name, "install_skill") == 0 || strcmp(name, "memory") == 0 ||
           strcmp(name, "ask_user_question") == 0;
}

char *tools_schema_json(tools_env *env) {
    if (env && env->ctx && (env->ctx->mcp_disabled || env->ctx->library_mode)) {
        yyjson_doc *doc = jparse(SCHEMA_JSON, strlen(SCHEMA_JSON));
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        yyjson_mut_doc *mut = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *out = mut ? yyjson_mut_arr(mut) : NULL;
        if (root && out) {
            size_t idx, max;
            yyjson_val *item;
            yyjson_arr_foreach(root, idx, max, item) {
                const char *name = jget_str(jget(item, "function"), "name");
                if (schema_tool_disabled(env, name)) continue;
                yyjson_mut_arr_add_val(out, yyjson_val_mut_copy(mut, item));
            }
            yyjson_mut_doc_set_root(mut, out);
            char *json = jwrite(mut);
            yyjson_mut_doc_free(mut);
            yyjson_doc_free(doc);
            if (json) return json;
        } else {
            yyjson_mut_doc_free(mut);
            yyjson_doc_free(doc);
        }
    }
    return xstrdup(SCHEMA_JSON);
}

static bool json_type_matches(yyjson_val *value, const char *type) {
    if (!value || !type) return false;
    if (strcmp(type, "string") == 0) return yyjson_is_str(value);
    if (strcmp(type, "integer") == 0) return yyjson_is_int(value) || yyjson_is_uint(value);
    if (strcmp(type, "number") == 0) return yyjson_is_num(value);
    if (strcmp(type, "boolean") == 0) return yyjson_is_bool(value);
    if (strcmp(type, "object") == 0) return yyjson_is_obj(value);
    if (strcmp(type, "array") == 0) return yyjson_is_arr(value);
    return true; /* unknown future schema type is validated by the tool */
}

static int validate_call_schema(const char *name, yyjson_val *args,
                                char **error) {
    if (!args || !yyjson_is_obj(args)) {
        *error = tool_err("arguments for %s must be a JSON object", name);
        return -1;
    }
    yyjson_doc *schemas = jparse(SCHEMA_JSON, strlen(SCHEMA_JSON));
    yyjson_val *root = schemas ? yyjson_doc_get_root(schemas) : NULL;
    yyjson_val *parameters = NULL;
    if (root && yyjson_is_arr(root)) {
        size_t idx, max;
        yyjson_val *item;
        yyjson_arr_foreach(root, idx, max, item) {
            yyjson_val *function = jget(item, "function");
            const char *candidate = jget_str(function, "name");
            if (candidate && strcmp(candidate, name) == 0) {
                parameters = jget(function, "parameters");
                break;
            }
        }
    }
    if (!parameters) {
        yyjson_doc_free(schemas);
        *error = tool_err("unknown tool %s", name);
        return -1;
    }
    yyjson_val *required = jget(parameters, "required");
    if (required && yyjson_is_arr(required)) {
        size_t idx, max;
        yyjson_val *field;
        yyjson_arr_foreach(required, idx, max, field) {
            const char *key = yyjson_get_str(field);
            if (key && !jget(args, key)) {
                *error = tool_err("%s needs argument %s", name, key);
                yyjson_doc_free(schemas);
                return -1;
            }
        }
    }
    yyjson_val *properties = jget(parameters, "properties");
    if (properties && yyjson_is_obj(properties)) {
        size_t idx, max;
        yyjson_val *key_value, *schema;
        yyjson_obj_foreach(properties, idx, max, key_value, schema) {
            const char *key = yyjson_get_str(key_value);
            yyjson_val *value = key ? jget(args, key) : NULL;
            if (!value) continue;
            const char *type = jget_str(schema, "type");
            if (type && !json_type_matches(value, type)) {
                *error = tool_err("%s argument %s must be %s", name, key, type);
                yyjson_doc_free(schemas);
                return -1;
            }
        }
    }
    yyjson_doc_free(schemas);
    return 0;
}

/* Extract one path-like human detail used for permission rules/prompts. */
static char *path_detail(tools_env *env, yyjson_val *args, const char *field) {
    const char *p = jget_str(args, field);
    if (!p) return NULL;
    if (p[0] == '/') return xstrdup(p);
    if (env->ctx->ssh_host) {
        buf_t b;
        buf_init(&b);
        buf_appendf(&b, "%s/%s", env->ctx->ssh_cwd ? env->ctx->ssh_cwd : "", p);
        return buf_detach(&b);
    }
    char *err = NULL;
    char *abs = tool_resolve_path(env, p, &err);
    free(err);
    return abs;
}

/* Extract the primary human detail used for permission rules and prompts. */
static char *call_detail(tools_env *env, const char *name, yyjson_val *args) {
    if (strcmp(name, "terminal") == 0 || strcmp(name, "run_command") == 0) {
        const char *cmd = jget_str(args, "command");
        return cmd ? xstrdup(cmd) : NULL;
    }
    char *detail = path_detail(env, args, "path");
    if (detail) return detail;
    const char *url = jget_str(args, "url");
    return url ? xstrdup(url) : NULL;
}

static const char *canonical_name(const char *name) {
    if (strcmp(name, "run_command") == 0) return "terminal"; /* fx alias */
    if (strcmp(name, "vision") == 0) return "read_image";   /* fx name */
    return name;
}

int tools_call_prepare(tools_env *env, const char *name,
                       const char *args_json, tools_call *call) {
    if (!env || !name || !call) return -1;
    memset(call, 0, sizeof *call);
    call->name = xstrdup(canonical_name(name));
    call->permission_tool = xstrdup(call->name);
    call->doc = args_json ? jparse(args_json, strlen(args_json)) : NULL;
    call->args = call->doc ? yyjson_doc_get_root(call->doc) : NULL;
    if (validate_call_schema(call->name, call->args, &call->error) != 0)
        return -1;
    if (strcmp(call->name, "mcp_select_tool") == 0) {
        const char *server = jget_str(call->args, "server");
        const char *tool = jget_str(call->args, "tool");
        free(call->permission_tool);
        buf_t identity;
        buf_init(&identity);
        buf_appendf(&identity, "mcp:%s/%s", server, tool);
        call->permission_tool = buf_detach(&identity);
    }
    call->detail = call_detail(env, call->name, call->args);
    if (strcmp(call->name, "rename_file") == 0 ||
        strcmp(call->name, "copy_file") == 0)
        call->detail2 = path_detail(env, call->args, "new_path");
    call->verdict = perm_check(env->perm, call->permission_tool, call->detail);
    if (call->detail2) {
        perm_verdict second = perm_check(env->perm, call->permission_tool,
                                         call->detail2);
        if (second == PERM_DENY || call->verdict == PERM_DENY)
            call->verdict = PERM_DENY;
        else if (second == PERM_PROMPT || call->verdict == PERM_PROMPT)
            call->verdict = PERM_PROMPT;
    }
    if (call->verdict == PERM_PROMPT) {
        buf_t summary;
        buf_init(&summary);
        buf_appendf(&summary, "%s %s", call->permission_tool,
                    call->detail ? call->detail : "");
        if (call->detail2) buf_appendf(&summary, " -> %s", call->detail2);
        call->summary = buf_detach(&summary);
    }
    return 0;
}

void tools_call_grant(tools_env *env, const tools_call *call) {
    if (!env || !call) return;
    perm_grant(env->perm, call->permission_tool, call->detail);
    if (call->detail2)
        perm_grant(env->perm, call->permission_tool, call->detail2);
}

char *tools_call_execute(tools_env *env, const tools_call *call) {
    const char *name = call->name;
    yyjson_val *args = call->args;

    bool handled;
    char *out = tool_ssh_execute(env, name, args, &handled);
    if (!handled) out = tool_fs_execute(env, name, args, &handled);
    if (!handled) out = tool_shell_execute(env, name, args, &handled);
    if (!handled) out = tool_web_execute(env, name, args, &handled);
    if (!handled) out = tool_ext_execute(env, name, args, &handled);
    if (!handled) out = tool_err("unknown tool %s", name);
    return out;
}

void tools_call_free(tools_call *call) {
    if (!call) return;
    free(call->name);
    free(call->permission_tool);
    free(call->detail);
    free(call->detail2);
    free(call->summary);
    free(call->error);
    yyjson_doc_free(call->doc);
    memset(call, 0, sizeof *call);
}

int tools_flush_images(tools_env *env, char *err, size_t errlen) {
    if (!env || env->n_pending_images <= 0) return 0;
    env->pending_images[env->n_pending_images] = NULL;
    int rc = session_add_user_images(env->session, "Image attached by read_image.",
                                     (const char **)env->pending_images, err, errlen);
    for (int i = 0; i < env->n_pending_images; i++) {
        free(env->pending_images[i]);
        env->pending_images[i] = NULL;
    }
    env->n_pending_images = 0;
    return rc;
}

char *tools_execute(tools_env *env, const char *name, const char *args_json) {
    name = canonical_name(name);
    tools_call call;
    if (tools_call_prepare(env, name, args_json, &call) != 0) {
        char *out = call.error ? xstrdup(call.error)
                               : tool_err("cannot prepare tool call %s", name);
        tools_call_free(&call);
        return out;
    }
    perm_verdict v = call.verdict;
    if (v == PERM_PROMPT && env->prompt) {
        tny_perm_decision d = env->prompt(call.name, call.summary, env->prompt_ud);
        if (d == TNY_PERM_DECISION_ALLOW_ALWAYS) {
            tools_call_grant(env, &call);
            v = PERM_ALLOW;
        } else if (d == TNY_PERM_DECISION_ALLOW) {
            v = PERM_ALLOW;
        } else {
            v = PERM_DENY;
        }
    }
    if (v == PERM_PROMPT) {
        env->perm_blocked = true;
        char *out = tool_err("permission required for %s and no reviewer is available "
                             "(run with --auto, --yolo, or grant a rule)", call.name);
        tools_call_free(&call);
        return out;
    }
    if (v == PERM_DENY) {
        char *out = tool_err("permission denied for %s", call.name);
        tools_call_free(&call);
        return out;
    }
    char *out = tools_call_execute(env, &call);
    tools_call_free(&call);
    return out;
}
