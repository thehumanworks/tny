/* intercept.c — see intercept.h. Two halves: a narrow recogniser for the
 * documented `tny` command shapes, and an executor that reuses the typed
 * tools' own code paths so permissions, undo, MCP, and --ssh behave
 * identically to a structured tool call (docs/adr/0063). */
#include "core/intercept.h"
#include "core/edit.h"
#include "core/shellwords.h"
#include "mcp/mcp.h"
#include "util/util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- recogniser ---- */

static bool is_tny_argv0(const char *word) {
    const char *slash = strrchr(word, '/');
    return strcmp(slash ? slash + 1 : word, "tny") == 0;
}

/* `printf FORMAT` with no conversion specifications: the documented way to
 * feed an edit fence. Anything needing real printf semantics is refused. */
static bool printf_expand(const char *format, buf_t *out) {
    if (strchr(format, '%')) return false;
    for (const char *p = format; *p; p++) {
        if (*p != '\\' || !p[1]) {
            buf_append(out, p, 1);
            continue;
        }
        p++;
        switch (*p) {
        case 'n': buf_appends(out, "\n"); break;
        case 't': buf_appends(out, "\t"); break;
        case 'r': buf_appends(out, "\r"); break;
        case 'a': buf_appends(out, "\a"); break;
        case 'b': buf_appends(out, "\b"); break;
        case 'f': buf_appends(out, "\f"); break;
        case 'v': buf_appends(out, "\v"); break;
        case '\\': buf_appends(out, "\\"); break;
        /* \0nnn, \xhh and \c differ between shells' printf: refuse */
        default: return false;
        }
    }
    return !buf_oom(out);
}

/* `echo [-n] WORD…`. A backslash is refused: dash expands it, bash does not. */
static bool echo_expand(char **argv, int argc, buf_t *out) {
    int i = 1;
    bool newline = true;
    if (i < argc && strcmp(argv[i], "-n") == 0) {
        newline = false;
        i++;
    }
    for (int first = i; i < argc; i++) {
        if (strchr(argv[i], '\\')) return false;
        if (i > first) buf_appends(out, " ");
        buf_appends(out, argv[i]);
    }
    if (newline) buf_appends(out, "\n");
    return !buf_oom(out);
}

/* `cat FILE`, restricted to a readable file inside the allowed roots so the
 * intercept never reads what the sandboxed shell child could not. */
static bool cat_expand(tools_env *env, char **argv, int argc, buf_t *out) {
    if (argc != 2 || argv[1][0] == '-') return false;
    char *resolve_error = NULL;
    char *abs = tool_resolve_path(env, argv[1], &resolve_error);
    free(resolve_error);
    if (!abs) return false;
    if (env->ctx->ssh_host || !perm_path_allowed(env->ctx, abs)) {
        free(abs);
        return false;
    }
    size_t len = 0;
    char *data = file_slurp(abs, &len);
    free(abs);
    if (!data) return false;
    buf_append(out, data, len);
    free(data);
    return !buf_oom(out);
}

static bool producer_output(tools_env *env, const tny_words *w, buf_t *out) {
    if (w->argc < 1) return false;
    if (strcmp(w->argv[0], "printf") == 0) return w->argc == 2 && printf_expand(w->argv[1], out);
    if (strcmp(w->argv[0], "echo") == 0) return echo_expand(w->argv, w->argc, out);
    if (strcmp(w->argv[0], "cat") == 0) return cat_expand(env, w->argv, w->argc, out);
    return false;
}

/* `<<'EOF'` / `<<"EOF"` / `<<EOF`, then the body, then the delimiter alone on
 * its line and nothing else. An unquoted delimiter is accepted only when the
 * body has nothing the shell would expand. */
static bool heredoc_body(const char *p, buf_t *out) {
    if (p[0] != '<' || p[1] != '<' || p[2] == '<' || p[2] == '-') return false;
    p += 2;
    while (*p == ' ' || *p == '\t') p++;
    char quote = (*p == '\'' || *p == '"') ? *p : 0;
    buf_t delimiter;
    buf_init(&delimiter);
    if (quote) {
        const char *end = strchr(++p, quote);
        if (!end) {
            buf_free(&delimiter);
            return false;
        }
        buf_append(&delimiter, p, (size_t)(end - p));
        p = end + 1;
    } else {
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && !tny_shellword_meta(*p)) p++;
        buf_append(&delimiter, start, (size_t)(p - start));
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '\n' || !delimiter.len || buf_oom(&delimiter)) {
        buf_free(&delimiter);
        return false;
    }
    p++;

    bool closed = false;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len == delimiter.len && memcmp(p, delimiter.data, len) == 0) {
            p = eol ? eol + 1 : p + len;
            closed = true;
            break;
        }
        buf_append(out, p, len);
        buf_appends(out, "\n");
        if (!eol) break;
        p = eol + 1;
    }
    bool ok = closed && !buf_oom(out);
    for (; ok && *p; p++)
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') ok = false;
    if (ok && !quote)
        ok = out->len == 0 ||
             (!memchr(out->data, '$', out->len) && !memchr(out->data, '`', out->len) &&
              !memchr(out->data, '\\', out->len));
    buf_free(&delimiter);
    return ok;
}

static tny_intercept *ic_new(tny_intercept_kind kind, const char *permission_tool) {
    tny_intercept *ic = calloc(1, sizeof *ic);
    if (!ic) return NULL;
    ic->kind = kind;
    ic->permission_tool = xstrdup(permission_tool);
    if (!ic->permission_tool) {
        tny_intercept_free(ic);
        return NULL;
    }
    return ic;
}

static tny_intercept *ic_label(tny_intercept *ic, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static tny_intercept *ic_label(tny_intercept *ic, const char *fmt, ...) {
    if (!ic) return NULL;
    buf_t label;
    buf_init(&label);
    va_list ap;
    va_start(ap, fmt);
    char line[1024];
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    buf_appends(&label, line);
    ic->label = buf_detach(&label);
    if (!ic->label) {
        tny_intercept_free(ic);
        return NULL;
    }
    return ic;
}

static tny_intercept *parse_edit(tools_env *env, char **argv, int argc, int i, bool json) {
    const char *marker = "***";
    const char *path = NULL;
    bool positional = false;
    for (; i < argc; i++) {
        const char *arg = argv[i];
        if (!positional && strcmp(arg, "--") == 0) {
            positional = true;
        } else if (!positional && strcmp(arg, "--json") == 0) {
            json = true;
        } else if (!positional && strcmp(arg, "--marker") == 0) {
            if (++i >= argc) return NULL;
            marker = argv[i];
        } else if (!positional && arg[0] == '-') {
            return NULL; /* --help and unknown options: let the real CLI answer */
        } else if (path) {
            return NULL;
        } else {
            path = arg;
        }
    }
    if (!path || !*path || !*marker || strchr(marker, '\n') || strchr(marker, '\r')) return NULL;
    tny_intercept *ic = ic_new(TNY_INTERCEPT_EDIT, "edit_file");
    if (!ic) return NULL;
    ic->json = json;
    ic->target = xstrdup(path);
    ic->marker = xstrdup(marker);
    ic->detail = tools_path_detail(env, path);
    if (!ic->target || !ic->marker || !ic->detail) {
        tny_intercept_free(ic);
        return NULL;
    }
    return ic_label(ic, "tny edit %s", path);
}

/* `tny mcp tools SERVER` / `tny mcp describe SERVER/TOOL`: catalog reads
 * answered by the warmed client (docs/adr/0068). Identity is the native
 * mcp_search_tools meta-tool, which is what they stand for. */
static tny_intercept *parse_mcp_describe(char **argv, int argc, int i, bool json) {
    const char *sub = argv[i++];
    bool describe = strcmp(sub, "describe") == 0;
    const char *spec = NULL;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = true;
        else if (spec) return NULL;
        else spec = argv[i];
    }
    if (!spec || !*spec) return NULL;
    const char *slash = strchr(spec, '/');
    if (describe) {
        if (!slash || slash == spec || !slash[1] || strchr(slash + 1, '/')) return NULL;
    } else if (slash) {
        return NULL;
    }
    tny_intercept *ic = ic_new(TNY_INTERCEPT_MCP_DESCRIBE, "mcp_search_tools");
    if (!ic) return NULL;
    ic->json = json;
    ic->action = xstrdup(sub);
    ic->target = xstrdup(spec);
    if (!ic->action || !ic->target) {
        tny_intercept_free(ic);
        return NULL;
    }
    return ic_label(ic, "tny mcp %s %s", sub, spec);
}

static tny_intercept *parse_mcp(char **argv, int argc, int i, bool json) {
    const char *spec = NULL;
    if (i >= argc) return NULL;
    if (strcmp(argv[i], "tools") == 0 || strcmp(argv[i], "describe") == 0)
        return parse_mcp_describe(argv, argc, i, json);
    if (strcmp(argv[i], "call") != 0) return NULL;
    for (i++; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = true;
        else if (spec) return NULL;
        else spec = argv[i];
    }
    const char *slash = spec ? strchr(spec, '/') : NULL;
    if (!slash || slash == spec || !slash[1] || strchr(slash + 1, '/')) return NULL;
    buf_t identity;
    buf_init(&identity);
    buf_appendf(&identity, "mcp:%s", spec);
    char *permission_tool = buf_detach(&identity);
    tny_intercept *ic = permission_tool ? ic_new(TNY_INTERCEPT_MCP_CALL, permission_tool) : NULL;
    free(permission_tool);
    if (!ic) return NULL;
    ic->json = json;
    ic->target = xstrdup(spec);
    if (!ic->target) {
        tny_intercept_free(ic);
        return NULL;
    }
    return ic_label(ic, "tny mcp call %s", spec);
}

static tny_intercept *parse_memory(char **argv, int argc, int i, bool has_payload) {
    if (i >= argc) return NULL;
    const char *action = argv[i++];
    const char *key = i < argc ? argv[i++] : NULL;
    const char *value = i < argc ? argv[i++] : NULL;
    if (i != argc) return NULL;
    bool get = strcmp(action, "get") == 0, set = strcmp(action, "set") == 0;
    if (!get && !set && strcmp(action, "list") != 0) return NULL;
    if ((get || set) && !key) return NULL;
    if (!set && value) return NULL;
    if (set && !value && !has_payload) return NULL;
    if (!get && !set && key) return NULL;
    tny_intercept *ic = ic_new(TNY_INTERCEPT_MEMORY, "memory");
    if (!ic) return NULL;
    ic->action = xstrdup(action);
    ic->target = key ? xstrdup(key) : NULL;
    ic->value = value ? xstrdup(value) : NULL;
    if (!ic->action || (key && !ic->target) || (value && !ic->value)) {
        tny_intercept_free(ic);
        return NULL;
    }
    return ic_label(ic, "tny memory %s%s%s", action, key ? " " : "", key ? key : "");
}

static tny_intercept *parse_skill(char **argv, int argc, int i) {
    if (argc - i != 2 || strcmp(argv[i], "show") != 0) return NULL;
    tny_intercept *ic = ic_new(TNY_INTERCEPT_SKILL, "skill");
    if (!ic) return NULL;
    ic->target = xstrdup(argv[i + 1]);
    if (!ic->target) {
        tny_intercept_free(ic);
        return NULL;
    }
    return ic_label(ic, "tny skill show %s", argv[i + 1]);
}

static tny_intercept *parse_image(char **argv, int argc, int i, bool json) {
    const char *path = NULL;
    if (i >= argc || strcmp(argv[i], "attach") != 0) return NULL;
    for (i++; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = true;
        else if (path) return NULL;
        else path = argv[i];
    }
    if (!path || !*path) return NULL;
    tny_intercept *ic = ic_new(TNY_INTERCEPT_IMAGE_ATTACH, "read_image");
    if (!ic) return NULL;
    ic->json = json;
    ic->target = xstrdup(path);
    if (!ic->target) {
        tny_intercept_free(ic);
        return NULL;
    }
    return ic_label(ic, "tny image attach %s", path);
}

static tny_intercept *parse_ask_user(char **argv, int argc, int i, bool json,
                                     const buf_t *payload) {
    buf_t question;
    buf_init(&question);
    bool positional = false;
    for (; i < argc; i++) {
        const char *arg = argv[i];
        if (!positional && strcmp(arg, "--") == 0) {
            positional = true;
        } else if (!positional && strcmp(arg, "--json") == 0) {
            json = true;
        } else if (!positional && arg[0] == '-') {
            buf_free(&question);
            return NULL;
        } else {
            if (question.len) buf_appends(&question, " ");
            buf_appends(&question, arg);
            positional = true;
        }
    }
    if (!question.len && payload) buf_append(&question, payload->data, payload->len);
    if (!question.len || buf_oom(&question)) {
        buf_free(&question);
        return NULL;
    }
    tny_intercept *ic = ic_new(TNY_INTERCEPT_ASK_USER, "ask_user_question");
    if (!ic) {
        buf_free(&question);
        return NULL;
    }
    ic->json = json;
    ic->value = buf_detach(&question);
    if (!ic->value) {
        tny_intercept_free(ic);
        return NULL;
    }
    return ic_label(ic, "tny ask-user");
}

/* A foreground nested agent has no place inside a turn: it would run its own
 * loop under this one, invisible to the frontend and to cancellation. */
static tny_intercept *parse_ask(char **argv, int argc, int i) {
    for (; i < argc; i++)
        if (strcmp(argv[i], "-B") == 0 || strcmp(argv[i], "--background") == 0) return NULL;
    tny_intercept *ic = ic_new(TNY_INTERCEPT_REFUSED, "terminal");
    if (!ic) return NULL;
    ic->message =
        xstrdup("a foreground nested `tny ask` is not allowed inside a turn; start it in the "
                "background with `tny ask -B \"…\"` and collect it with "
                "`tny session ID --wait --json`");
    if (!ic->message) {
        tny_intercept_free(ic);
        return NULL;
    }
    return ic_label(ic, "tny ask");
}

static tny_intercept *parse_verb(tools_env *env, const tny_words *w, const buf_t *payload) {
    char **argv = w->argv;
    int argc = w->argc;
    if (argc < 2 || !is_tny_argv0(argv[0])) return NULL;
    int i = 1;
    bool json = false;
    while (i < argc && strcmp(argv[i], "--json") == 0) {
        json = true;
        i++;
    }
    if (i >= argc) return NULL;
    const char *verb = argv[i++];
    if (strcmp(verb, "edit") == 0) return parse_edit(env, argv, argc, i, json);
    if (strcmp(verb, "mcp") == 0) return parse_mcp(argv, argc, i, json);
    if (strcmp(verb, "memory") == 0) return parse_memory(argv, argc, i, payload != NULL);
    if (strcmp(verb, "skill") == 0) return parse_skill(argv, argc, i);
    if (strcmp(verb, "image") == 0) return parse_image(argv, argc, i, json);
    if (strcmp(verb, "ask-user") == 0) return parse_ask_user(argv, argc, i, json, payload);
    if (strcmp(verb, "ask") == 0) return parse_ask(argv, argc, i);
    return NULL;
}

tny_intercept *tny_intercept_parse(tools_env *env, const char *command) {
    if (!env || !env->ctx || !command || !*command) return NULL;
    tny_words words;
    if (tny_shellwords(command, &words) != 0) return NULL;

    tny_words piped = {0};
    const tny_words *verb_words = &words;
    buf_t payload;
    buf_init(&payload);
    bool has_payload = false;
    bool ok = true;
    if (words.stop == '<') {
        ok = heredoc_body(command + words.consumed, &payload);
        has_payload = ok;
    } else if (words.stop == '|' && command[words.consumed + 1] != '|') {
        ok = producer_output(env, &words, &payload);
        has_payload = ok;
        if (ok) ok = tny_shellwords(command + words.consumed + 1, &piped) == 0 && piped.stop == 0;
        verb_words = &piped;
    } else if (words.stop != 0) {
        ok = false;
    }

    tny_intercept *ic = ok ? parse_verb(env, verb_words, has_payload ? &payload : NULL) : NULL;
    if (ic && has_payload) {
        ic->stdin_len = payload.len;
        ic->stdin_data = buf_detach(&payload);
        if (!ic->stdin_data) {
            tny_intercept_free(ic);
            ic = NULL;
        }
    }
    buf_free(&payload);
    tny_shellwords_free(&piped);
    tny_shellwords_free(&words);
    return ic;
}

void tny_intercept_free(tny_intercept *ic) {
    if (!ic) return;
    free(ic->label);
    free(ic->permission_tool);
    free(ic->detail);
    free(ic->target);
    free(ic->action);
    free(ic->value);
    free(ic->marker);
    free(ic->stdin_data);
    free(ic->message);
    free(ic);
}

/* ---- executor ---- */

static char *ic_result(tools_env *env, int code, buf_t *out, buf_t *err) {
    buf_t result;
    buf_init(&result);
    buf_appendf(&result, "exit: %d\n", code);
    if (out->len) buf_append(&result, out->data, out->len);
    if (err->len) buf_append(&result, err->data, err->len);
    buf_free(out);
    buf_free(err);
    char *bounded = result.data ? tool_bound_result(env, result.data, result.len) : NULL;
    buf_free(&result);
    return bounded;
}

static void ic_record_undo(const char *resolved_path, void *userdata) {
    tools_undo_record(userdata, resolved_path);
}

static char *exec_edit(tools_env *env, const tny_intercept *ic) {
    buf_t out, err;
    buf_init(&out);
    buf_init(&err);
    tny_edit_payload payload;
    char parse_error[256];
    if (!tny_edit_parse_payload(ic->stdin_data, ic->stdin_len, ic->json, ic->marker, &payload,
                                parse_error, sizeof parse_error)) {
        tny_edit_usage(parse_error, &err);
        return ic_result(env, 1, &out, &err);
    }
    tny_edit_result result = {0};
    tny_edit_status status;
    char *transport_error = NULL;
    if (env->ctx->ssh_host) {
        status = tool_ssh_edit_exact(env, ic->target, payload.old_text, payload.new_text,
                                     payload.replace_all, &result, &transport_error);
    } else {
        tny_edit_hooks hooks = {.before_write = ic_record_undo, .before_write_userdata = env};
        status = tny_edit_file_exact(ic->detail, payload.old_text, payload.new_text,
                                     payload.replace_all, &hooks, &result);
    }
    tny_edit_payload_free(&payload);
    int code;
    if (transport_error) {
        buf_appendf(&err, "tny: edit: %s\n", transport_error);
        free(transport_error);
        code = 1;
    } else {
        code = tny_edit_render(ic->target, ic->json, status, &result, &out, &err);
    }
    tny_edit_result_free(&result);
    return ic_result(env, code, &out, &err);
}

static char *exec_mcp_call(tools_env *env, const tny_intercept *ic) {
    buf_t out, err;
    buf_init(&out);
    buf_init(&err);
    const char *arguments = ic->stdin_data && ic->stdin_len ? ic->stdin_data : "{}";
    yyjson_doc *doc = jparse(arguments, strlen(arguments));
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool object = root && yyjson_is_obj(root);
    yyjson_doc_free(doc);
    if (!object) {
        buf_appends(&err, "tny: mcp call: arguments on stdin must be one JSON object "
                          "(empty stdin means {})\n");
        return ic_result(env, 1, &out, &err);
    }
    char *server = xstrndup(ic->target, (size_t)(strchr(ic->target, '/') - ic->target));
    if (!server) {
        buf_free(&out);
        buf_free(&err);
        return NULL;
    }
    mcp_call_status status = MCP_CALL_CONFIG_ERROR;
    char *text = mcp_call_tool_raw(env, server, strchr(ic->target, '/') + 1, arguments, &status);
    if (!text) {
        free(server);
        buf_free(&out);
        buf_free(&err);
        return NULL;
    }
    size_t len = strlen(text);
    int code = 0;
    if (status != MCP_CALL_OK) {
        while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) text[--len] = '\0';
        buf_appendf(&err, "tny: mcp call: %s\n", str_starts(text, "error: ") ? text + 7 : text);
        code = status == MCP_CALL_CONFIG_ERROR ? 1 : 2;
        /* wrong arguments are the common failure: hand over the schema so
         * the retry is informed (docs/adr/0068) */
        char *schema = status == MCP_CALL_TOOL_ERROR
                           ? mcp_tool_schema(server, strchr(ic->target, '/') + 1)
                           : NULL;
        if (schema)
            buf_appendf(&err, "tny: mcp call: input schema for %s: %s\n", ic->target, schema);
        free(schema);
    } else if (len) {
        buf_append(&out, text, len);
        if (text[len - 1] != '\n') buf_appends(&out, "\n");
    }
    free(server);
    free(text);
    return ic_result(env, code, &out, &err);
}

static char *exec_mcp_describe(tools_env *env, const tny_intercept *ic) {
    buf_t out, err;
    buf_init(&out);
    buf_init(&err);
    const char *slash = strchr(ic->target, '/');
    char *server = slash ? xstrndup(ic->target, (size_t)(slash - ic->target)) : xstrdup(ic->target);
    if (!server) {
        buf_free(&out);
        buf_free(&err);
        return NULL;
    }
    mcp_call_status status = MCP_CALL_CONFIG_ERROR;
    char *text = mcp_describe(env, server, slash ? slash + 1 : NULL, ic->json, &status);
    free(server);
    if (!text) {
        buf_free(&out);
        buf_free(&err);
        return NULL;
    }
    size_t len = strlen(text);
    int code = 0;
    if (status != MCP_CALL_OK) {
        while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) text[--len] = '\0';
        buf_appendf(&err, "tny: mcp %s: %s\n", ic->action,
                    str_starts(text, "error: ") ? text + 7 : text);
        code = status == MCP_CALL_CONFIG_ERROR ? 1 : 2;
    } else if (len) {
        buf_append(&out, text, len);
    }
    free(text);
    return ic_result(env, code, &out, &err);
}

/* memory and skill run the built-in tool implementations unchanged. */
static char *exec_ext(tools_env *env, const char *tool, const char *args_json) {
    yyjson_doc *doc = jparse(args_json, strlen(args_json));
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool handled = false;
    char *text = root ? tool_ext_execute(env, tool, root, &handled) : NULL;
    yyjson_doc_free(doc);
    if (!handled || !text) {
        free(text);
        return NULL;
    }
    return text;
}

static char *exec_memory(tools_env *env, const tny_intercept *ic) {
    buf_t args;
    buf_init(&args);
    buf_appends(&args, "{\"action\":");
    jescape(&args, ic->action);
    if (ic->target) {
        buf_appends(&args, ",\"key\":");
        jescape(&args, ic->target);
    }
    if (ic->value || ic->stdin_data) {
        buf_appends(&args, ",\"value\":");
        if (ic->value) jescape(&args, ic->value);
        else jescape(&args, ic->stdin_data);
    }
    buf_appends(&args, "}");
    if (buf_oom(&args)) {
        buf_free(&args);
        return NULL;
    }
    char *text = exec_ext(env, "memory", args.data);
    buf_free(&args);
    if (!text) return NULL;
    buf_t out, err;
    buf_init(&out);
    buf_init(&err);
    bool failed = str_starts(text, "error: ");
    buf_appends(failed ? &err : &out, failed ? text + 7 : text);
    buf_appends(failed ? &err : &out, "\n");
    free(text);
    return ic_result(env, failed ? 2 : 0, &out, &err);
}

static char *exec_skill(tools_env *env, const tny_intercept *ic) {
    buf_t args;
    buf_init(&args);
    buf_appends(&args, "{\"name\":");
    jescape(&args, ic->target);
    buf_appends(&args, "}");
    if (buf_oom(&args)) {
        buf_free(&args);
        return NULL;
    }
    char *text = exec_ext(env, "skill", args.data);
    buf_free(&args);
    if (!text) return NULL;
    buf_t out, err;
    buf_init(&out);
    buf_init(&err);
    bool failed = str_starts(text, "error: ");
    buf_appends(failed ? &err : &out, failed ? text + 7 : text);
    if (text[0] && text[strlen(text) - 1] != '\n') buf_appends(failed ? &err : &out, "\n");
    free(text);
    return ic_result(env, failed ? 2 : 0, &out, &err);
}

static char *exec_image_attach(tools_env *env, const tny_intercept *ic) {
    buf_t out, err;
    buf_init(&out);
    buf_init(&err);
    char attach_error[512];
    if (tools_queue_image(env, ic->target, true, NULL, NULL, NULL, attach_error,
                          sizeof attach_error) != 0) {
        buf_appendf(&err, "tny: %s\n", attach_error);
        return ic_result(env, 2, &out, &err);
    }
    if (ic->json) {
        char *id = gen_id();
        buf_appendf(&out, "{\"kind\":\"image_attach\",\"id\":\"%s\",\"ok\":true}\n", id ? id : "0");
        free(id);
    }
    return ic_result(env, 0, &out, &err);
}

static char *exec_ask_user(tools_env *env, const tny_intercept *ic) {
    buf_t out, err;
    buf_init(&out);
    buf_init(&err);
    char *answer = env->ask_user ? env->ask_user(ic->value, env->ask_user_ud) : NULL;
    if (!answer) {
        buf_appends(&err, "tny: no interactive owner is attached\n");
        return ic_result(env, 2, &out, &err);
    }
    if (ic->json) {
        char *id = gen_id();
        buf_appendf(&out, "{\"kind\":\"ask_user\",\"id\":\"%s\",\"answer\":", id ? id : "0");
        free(id);
        jescape(&out, answer);
        buf_appends(&out, "}\n");
    } else {
        buf_appends(&out, answer);
        if (!*answer || answer[strlen(answer) - 1] != '\n') buf_appends(&out, "\n");
    }
    free(answer);
    return ic_result(env, 0, &out, &err);
}

char *tny_intercept_execute(tools_env *env, const tny_intercept *ic) {
    if (!env || !ic) return NULL;
    switch (ic->kind) {
    case TNY_INTERCEPT_EDIT: return exec_edit(env, ic);
    case TNY_INTERCEPT_MCP_CALL: return exec_mcp_call(env, ic);
    case TNY_INTERCEPT_MCP_DESCRIBE: return exec_mcp_describe(env, ic);
    case TNY_INTERCEPT_MEMORY: return exec_memory(env, ic);
    case TNY_INTERCEPT_SKILL: return exec_skill(env, ic);
    case TNY_INTERCEPT_IMAGE_ATTACH: return exec_image_attach(env, ic);
    case TNY_INTERCEPT_ASK_USER: return exec_ask_user(env, ic);
    case TNY_INTERCEPT_REFUSED: return tool_err("%s", ic->message);
    }
    return NULL;
}
