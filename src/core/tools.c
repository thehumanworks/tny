/* tools.c — registry, permission gate, dispatch, result bounding. */
#include "core/tools.h"
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
    char *abs;
    if (path[0] == '/') abs = path_abs(path);
    else {
        char *joined = path_join(env->ctx->cwd, path);
        abs = path_abs(joined);
        if (!abs) abs = joined; else free(joined);
    }
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
"{\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":\"Read a text file. Args: path, optional offset (line), limit (lines).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"path\"]}}},"
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

char *tools_schema_json(tools_env *env) {
    (void)env;
    return xstrdup(SCHEMA_JSON);
}

/* Extract the human "detail" used for permission rules and prompts. */
static char *call_detail(tools_env *env, const char *name, yyjson_val *args) {
    if (strcmp(name, "terminal") == 0 || strcmp(name, "run_command") == 0) {
        const char *cmd = jget_str(args, "command");
        return cmd ? xstrdup(cmd) : NULL;
    }
    const char *p = jget_str(args, "path");
    if (!p) p = jget_str(args, "url");
    if (!p) return NULL;
    if (p[0] == '/') return xstrdup(p);
    char *err = NULL;
    char *abs = tool_resolve_path(env, p, &err);
    free(err);
    return abs;
}

char *tools_execute(tools_env *env, const char *name, const char *args_json) {
    if (strcmp(name, "run_command") == 0) name = "terminal"; /* fx alias */

    yyjson_doc *adoc = args_json ? jparse(args_json, strlen(args_json)) : NULL;
    yyjson_val *args = adoc ? yyjson_doc_get_root(adoc) : NULL;

    /* permission gate */
    char *detail = call_detail(env, name, args);
    perm_verdict v = perm_check(env->perm, name, detail);
    if (v == PERM_PROMPT && env->prompt) {
        buf_t sum;
        buf_init(&sum);
        buf_appendf(&sum, "%s %s", name, detail ? detail : "");
        tny_perm_decision d = env->prompt(name, sum.data, env->prompt_ud);
        buf_free(&sum);
        if (d == TNY_PERM_DECISION_ALLOW_ALWAYS) {
            perm_grant(env->perm, name, detail);
            v = PERM_ALLOW;
        } else if (d == TNY_PERM_DECISION_ALLOW) {
            v = PERM_ALLOW;
        } else {
            v = PERM_DENY;
        }
    }
    if (v == PERM_PROMPT) {
        env->perm_blocked = true;
        free(detail);
        yyjson_doc_free(adoc);
        return tool_err("permission required for %s and no reviewer is available "
                        "(run with --auto, --yolo, or grant a rule)", name);
    }
    if (v == PERM_DENY) {
        free(detail);
        yyjson_doc_free(adoc);
        return tool_err("permission denied for %s", name);
    }
    free(detail);

    bool handled = false;
    char *out = tool_fs_execute(env, name, args, &handled);
    if (!handled) out = tool_shell_execute(env, name, args, &handled);
    if (!handled) out = tool_web_execute(env, name, args, &handled);
    if (!handled) out = tool_ext_execute(env, name, args, &handled);
    if (!handled) out = tool_err("unknown tool %s", name);
    yyjson_doc_free(adoc);
    return out;
}
