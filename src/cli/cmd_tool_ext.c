/* Python tool extensions on the command line (docs/extensions.md, ADR 0218):
 *
 *   tny jira projects list --status open     one extension tool, flags from
 *                                            its input schema
 *   tny jira --help                          the module's commands
 *   tny tool check | tny tool new NAME       diagnose and scaffold
 *   tny mcp serve [--all]                    the same tools over stdio MCP
 *
 * Every call goes through tools_execute, so permissions, result bounds and
 * cancellation match code mode exactly. */
#include "cli/cli.h"
#include "core/tool_extensions.h"
#include "core/tools.h"
#include "json/json.h"
#include "util/util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EXT_STDIN_MAX (16u * 1024u * 1024u)

/* ---- catalog helpers ---------------------------------------------------- */

static yyjson_doc *load_catalog(tny_ctx *ctx) {
    tools_env env = {.ctx = ctx};
    char *json = tools_catalog_json(&env);
    yyjson_doc *doc = json ? jparse(json, strlen(json)) : NULL;
    free(json);
    return doc;
}

static yyjson_val *find_fn(yyjson_doc *doc, const char *name) {
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(doc ? yyjson_doc_get_root(doc) : NULL, idx, max, item) {
        yyjson_val *fn = jget(item, "function");
        const char *n = jget_str(fn, "name");
        if (n && strcmp(n, name) == 0) return fn;
    }
    return NULL;
}

/* True when some tool is `prefix` or lies under `prefix.`. */
static bool has_prefix(yyjson_doc *doc, const char *prefix) {
    size_t len = strlen(prefix), idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(doc ? yyjson_doc_get_root(doc) : NULL, idx, max, item) {
        const char *n = jget_str(jget(item, "function"), "name");
        if (n && strncmp(n, prefix, len) == 0 && (n[len] == '\0' || n[len] == '.')) return true;
    }
    return false;
}

static const char *first_line(const char *text, int *len) {
    if (!text) text = "";
    const char *nl = strchr(text, '\n');
    *len = nl ? (int)(nl - text) : (int)strlen(text);
    return text;
}

static void print_spaced(FILE *f, const char *dotted) {
    for (const char *c = dotted; *c; c++) fputc(*c == '.' ? ' ' : *c, f);
}

/* ---- schema-driven flags ------------------------------------------------ */

/* The JSON type a flag value converts to: "string", "integer", "number",
 * "boolean", "array", "object" or "json" (anything else: parse as JSON). */
static const char *flag_type(yyjson_val *schema) {
    const char *type = jget_str(schema, "type");
    if (type) return type;
    yyjson_val *types = jget(schema, "type");
    size_t idx, max;
    yyjson_val *t;
    yyjson_arr_foreach(types, idx, max, t) {
        const char *s = yyjson_get_str(t);
        if (s && strcmp(s, "null") != 0) return s;
    }
    static const char *const unions[] = {"anyOf", "oneOf", NULL};
    for (int u = 0; unions[u]; u++) {
        yyjson_val *branches = jget(schema, unions[u]);
        yyjson_arr_foreach(branches, idx, max, t) {
            const char *s = jget_str(t, "type");
            if (s && strcmp(s, "null") != 0) return s;
        }
    }
    yyjson_val *first = yyjson_arr_get_first(jget(schema, "enum"));
    if (yyjson_is_str(first)) return "string";
    if (yyjson_is_int(first)) return "integer";
    if (yyjson_is_num(first)) return "number";
    if (yyjson_is_bool(first)) return "boolean";
    return "json";
}

static yyjson_val *array_items(yyjson_val *schema) {
    yyjson_val *items = jget(schema, "items");
    if (items) return items;
    size_t idx, max;
    yyjson_val *t;
    yyjson_arr_foreach(jget(schema, "anyOf"), idx, max, t) {
        if (jget(t, "items")) return jget(t, "items");
    }
    return NULL;
}

/* Property for a flag spelling: exact, or with '-' read as '_'. */
static const char *match_property(yyjson_val *props, const char *flag, yyjson_val **schema) {
    if (!yyjson_is_obj(props)) return NULL;
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(props, idx, max, k, v) {
        const char *key = yyjson_get_str(k);
        if (!key) continue;
        size_t i = 0;
        for (; key[i] && flag[i]; i++) {
            char a = key[i], b = flag[i];
            if (a != b && !(a == '_' && b == '-')) break;
        }
        if (!key[i] && !flag[i]) {
            *schema = v;
            return key;
        }
    }
    return NULL;
}

static yyjson_mut_val *convert(yyjson_mut_doc *doc, const char *type, const char *text, char *err,
                               size_t err_len, const char *flag) {
    if (strcmp(type, "string") == 0) return yyjson_mut_strcpy(doc, text);
    if (strcmp(type, "integer") == 0) {
        char *end = NULL;
        errno = 0;
        long long v = strtoll(text, &end, 10);
        if (!*text || *end || errno) {
            snprintf(err, err_len, "--%s takes an integer, not '%s'", flag, text);
            return NULL;
        }
        return yyjson_mut_sint(doc, v);
    }
    if (strcmp(type, "number") == 0) {
        char *end = NULL;
        double v = strtod(text, &end);
        if (!*text || *end) {
            snprintf(err, err_len, "--%s takes a number, not '%s'", flag, text);
            return NULL;
        }
        return yyjson_mut_real(doc, v);
    }
    if (strcmp(type, "boolean") == 0) {
        if (strcmp(text, "true") == 0 || strcmp(text, "1") == 0 || strcmp(text, "yes") == 0)
            return yyjson_mut_true(doc);
        if (strcmp(text, "false") == 0 || strcmp(text, "0") == 0 || strcmp(text, "no") == 0)
            return yyjson_mut_false(doc);
        snprintf(err, err_len, "--%s takes true or false, not '%s'", flag, text);
        return NULL;
    }
    yyjson_doc *parsed = jparse(text, strlen(text));
    if (!parsed) {
        if (strcmp(type, "json") == 0) return yyjson_mut_strcpy(doc, text);
        snprintf(err, err_len, "--%s takes JSON (%s)", flag, type);
        return NULL;
    }
    yyjson_mut_val *v = yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed));
    yyjson_doc_free(parsed);
    return v;
}

static char *read_stdin_all(void) {
    buf_t b;
    buf_init(&b);
    for (;;) {
        char chunk[8192];
        ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (size_t)n > EXT_STDIN_MAX - b.len) {
            buf_free(&b);
            return NULL;
        }
        if (!n) break;
        buf_append(&b, chunk, (size_t)n);
    }
    return b.data ? buf_detach(&b) : xstrdup("");
}

/* argv after the tool name → JSON arguments. NULL with err set on misuse. */
static char *build_args(yyjson_val *params, int argc, char **argv, char *err, size_t err_len) {
    yyjson_val *props = jget(params, "properties");
    yyjson_val *required = jget(params, "required");
    yyjson_mut_doc *doc = yyjson_mut_doc_new(jallocator());
    yyjson_mut_val *obj = doc ? yyjson_mut_obj(doc) : NULL;
    if (!obj) {
        yyjson_mut_doc_free(doc);
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, obj);
    size_t positional = 0;
    bool ok = true;
    for (int i = 0; ok && i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-") == 0) {
            char *text = read_stdin_all();
            yyjson_doc *in = text ? jparse(text, strlen(text)) : NULL;
            free(text);
            yyjson_val *root = in ? yyjson_doc_get_root(in) : NULL;
            if (!yyjson_is_obj(root)) {
                snprintf(err, err_len, "stdin must hold one JSON object");
                ok = false;
            } else {
                size_t idx, max;
                yyjson_val *k, *v;
                yyjson_obj_foreach(root, idx, max, k, v) {
                    const char *key = yyjson_get_str(k);
                    if (key && !yyjson_mut_obj_get(obj, key))
                        yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key),
                                           yyjson_val_mut_copy(doc, v));
                }
            }
            yyjson_doc_free(in);
            continue;
        }
        if (a[0] != '-' || a[1] != '-' || !a[2]) {
            /* Positionals fill required properties in order. */
            const char *key = yyjson_get_str(yyjson_arr_get(required, positional));
            yyjson_val *schema = key ? jget(props, key) : NULL;
            if (!key) {
                snprintf(err, err_len, "unexpected argument '%s'", a);
                ok = false;
                break;
            }
            positional++;
            yyjson_mut_val *v = convert(doc, flag_type(schema), a, err, err_len, key);
            if (!v) ok = false;
            else yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), v);
            continue;
        }
        char flag[256];
        const char *eq = strchr(a + 2, '=');
        size_t flen = eq ? (size_t)(eq - (a + 2)) : strlen(a + 2);
        if (flen >= sizeof flag) flen = sizeof flag - 1;
        memcpy(flag, a + 2, flen);
        flag[flen] = '\0';
        yyjson_val *schema = NULL;
        const char *key = match_property(props, flag, &schema);
        bool negated = false;
        if (!key && str_starts(flag, "no-")) {
            key = match_property(props, flag + 3, &schema);
            negated = key && strcmp(flag_type(schema), "boolean") == 0;
            if (!negated) key = NULL;
        }
        if (!key) {
            snprintf(err, err_len, "unknown option --%s (see --help)", flag);
            ok = false;
            break;
        }
        const char *type = flag_type(schema);
        yyjson_mut_val *value = NULL;
        if (negated) {
            value = yyjson_mut_false(doc);
        } else if (strcmp(type, "boolean") == 0 && !eq) {
            value = yyjson_mut_true(doc);
        } else {
            const char *text = eq ? eq + 1 : (i + 1 < argc ? argv[++i] : NULL);
            if (!text) {
                snprintf(err, err_len, "--%s needs a value", flag);
                ok = false;
                break;
            }
            if (strcmp(type, "array") == 0 && text[0] != '[') {
                /* Repeated flags append: --tag a --tag b. */
                yyjson_mut_val *arr = yyjson_mut_obj_get(obj, key);
                if (!arr || !yyjson_mut_is_arr(arr)) {
                    arr = yyjson_mut_arr(doc);
                    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), arr);
                }
                yyjson_mut_val *item =
                    convert(doc, flag_type(array_items(schema)), text, err, err_len, flag);
                if (!item) ok = false;
                else yyjson_mut_arr_append(arr, item);
                continue;
            }
            value = convert(doc, type, text, err, err_len, flag);
        }
        if (!value) ok = false;
        else yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), value);
    }
    char *json = ok ? jwrite(doc) : NULL;
    yyjson_mut_doc_free(doc);
    return json;
}

/* ---- help --------------------------------------------------------------- */

static void print_tool_help(yyjson_val *fn) {
    const char *name = jget_str(fn, "name");
    yyjson_val *params = jget(fn, "parameters");
    yyjson_val *props = jget(params, "properties");
    yyjson_val *required = jget(params, "required");
    fputs("Usage: tny ", stdout);
    print_spaced(stdout, name);
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_val *r;
    yyjson_arr_foreach(required, idx, max, r) printf(" %s", yyjson_get_str(r));
    fputs(" [OPTIONS] [-] [--json]\n\n", stdout);
    const char *desc = jget_str(fn, "description");
    if (desc && *desc) printf("%s\n\n", desc);
    if (!yyjson_is_obj(props) || !yyjson_obj_size(props)) {
        fputs("Takes no options.\n", stdout);
        return;
    }
    fputs("Options:\n", stdout);
    yyjson_obj_foreach(props, idx, max, k, v) {
        const char *key = yyjson_get_str(k);
        const char *type = flag_type(v);
        char left[160];
        yyjson_val *en = jget(v, "enum");
        if (strcmp(type, "boolean") == 0) {
            snprintf(left, sizeof left, "--%s, --no-%s", key, key);
        } else if (en && yyjson_is_arr(en)) {
            buf_t b;
            buf_init(&b);
            size_t i2, m2;
            yyjson_val *e;
            yyjson_arr_foreach(en, i2, m2, e) {
                if (i2) buf_appends(&b, "|");
                if (yyjson_is_str(e)) buf_appends(&b, yyjson_get_str(e));
                else {
                    char *t = jwrite_val(e);
                    buf_appends(&b, t ? t : "?");
                    free(t);
                }
            }
            snprintf(left, sizeof left, "--%s %s", key, b.data ? b.data : "");
            buf_free(&b);
        } else {
            const char *meta = strcmp(type, "integer") == 0  ? "N"
                               : strcmp(type, "number") == 0 ? "X"
                               : strcmp(type, "array") == 0  ? "VALUE..."
                               : strcmp(type, "string") == 0 ? "TEXT"
                                                             : "JSON";
            snprintf(left, sizeof left, "--%s %s", key, meta);
        }
        bool req = false;
        size_t i3, m3;
        yyjson_arr_foreach(required, i3, m3, r) {
            const char *rk = yyjson_get_str(r);
            if (rk && strcmp(rk, key) == 0) req = true;
        }
        printf("  %-28s", left);
        if (strlen(left) > 28)
            fputs("\n  "
                  "                            ",
                  stdout);
        const char *d = jget_str(v, "description");
        bool said = d && *d;
        if (said) fputs(d, stdout);
        yyjson_val *dflt = jget(v, "default");
        if (req) {
            fputs(said ? " (required)" : "(required)", stdout);
            said = true;
        } else if (dflt && !yyjson_is_null(dflt)) {
            char *t = jwrite_val(dflt);
            printf("%s(default: %s)", said ? " " : "", t ? t : "?");
            free(t);
            said = true;
        }
        if (strcmp(type, "array") == 0) fputs(said ? " Repeatable." : "Repeatable.", stdout);
        fputc('\n', stdout);
    }
    fputs("\nArguments may also come as one JSON object on stdin (-); flags override it.\n",
          stdout);
}

static void print_group_help(yyjson_doc *doc, const char *prefix) {
    size_t len = strlen(prefix), idx, max;
    printf("Usage: tny ");
    print_spaced(stdout, prefix);
    fputs(" COMMAND [OPTIONS]\n\nCommands:\n", stdout);
    yyjson_val *item;
    yyjson_arr_foreach(yyjson_doc_get_root(doc), idx, max, item) {
        yyjson_val *fn = jget(item, "function");
        const char *n = jget_str(fn, "name");
        if (!n || strncmp(n, prefix, len) != 0 || n[len] != '.') continue;
        char left[160];
        size_t j = 0;
        for (const char *c = n + len + 1; *c && j + 1 < sizeof left; c++)
            left[j++] = *c == '.' ? ' ' : *c;
        left[j] = '\0';
        int dl;
        const char *d = first_line(jget_str(fn, "description"), &dl);
        printf("  %-24s %.*s\n", left, dl, d);
    }
    fputs("\nRun tny ", stdout);
    print_spaced(stdout, prefix);
    fputs(" COMMAND --help for its options.\n", stdout);
}

/* ---- tny MODULE ... ----------------------------------------------------- */

int cmd_tool_ext(tny_ctx *ctx, const cli_globals *g, const char *module, int argc, char **argv) {
    if (!tny_tool_ext_enabled(ctx)) return 127;
    tny_tool_ext_set_stderr(STDERR_FILENO);
    yyjson_doc *doc = load_catalog(ctx);
    if (!doc || !has_prefix(doc, module)) {
        yyjson_doc_free(doc);
        return 127;
    }
    /* Longest prefix of positional words that names a tool or group. */
    buf_t name;
    buf_init(&name);
    buf_appends(&name, module);
    int i = 0;
    while (i < argc && argv[i][0] != '-' && !find_fn(doc, name.data)) {
        size_t keep = name.len;
        buf_appendf(&name, ".%s", argv[i]);
        if (!has_prefix(doc, name.data)) {
            name.len = keep;
            name.data[keep] = '\0';
            break;
        }
        i++;
    }
    bool json = g->json, help = false;
    int n = 0;
    for (int j = i; j < argc; j++) {
        if (strcmp(argv[j], "--json") == 0) json = true;
        else if (strcmp(argv[j], "--help") == 0 || strcmp(argv[j], "-h") == 0) help = true;
        else argv[i + n++] = argv[j];
    }
    yyjson_val *fn = find_fn(doc, name.data);
    int rc;
    if (!fn) {
        if (help || i == argc) {
            print_group_help(doc, name.data);
            rc = help ? 0 : 1;
        } else {
            fprintf(stderr, "tny: ");
            print_spaced(stderr, name.data);
            fprintf(stderr, ": unknown command '%s' (see tny ", argv[i]);
            print_spaced(stderr, name.data);
            fputs(" --help)\n", stderr);
            rc = 1;
        }
    } else if (help) {
        print_tool_help(fn);
        rc = 0;
    } else {
        char err[512] = "";
        char *args = build_args(jget(fn, "parameters"), n, argv + i, err, sizeof err);
        if (!args) {
            fprintf(stderr, "tny: ");
            print_spaced(stderr, name.data);
            fprintf(stderr, ": %s\n", err);
            rc = 1;
        } else {
            char *tool = xstrdup(name.data);
            yyjson_doc_free(doc);
            doc = NULL;
            rc = tool ? cmd_tool_session(ctx, "call", tool, args, json) : 1;
            free(tool);
            free(args);
        }
    }
    yyjson_doc_free(doc);
    buf_free(&name);
    return rc;
}

/* ---- tny tool check | new ----------------------------------------------- */

static void print_problem(yyjson_val *p, const char *kind) {
    const char *module = jget_str(p, "module"), *path = jget_str(p, "path");
    const char *msg = jget_str(p, kind);
    fprintf(stderr, "%s: %s%s%s%s%s\n", kind, module ? module : "", module ? ": " : "",
            msg ? msg : "?", path ? " — " : "", path ? path : "");
    const char *tb = jget_str(p, "traceback");
    if (tb) fprintf(stderr, "%s", tb);
}

int cmd_tool_check(tny_ctx *ctx, bool json) {
    if (!tny_tool_ext_enabled(ctx)) {
        fputs("tny: tool extensions are off (--no-tool-extensions, TNY_TOOL_EXTENSIONS=0 or "
              "settings extensions.tools.enabled)\n",
              stderr);
        return 1;
    }
    char *project = NULL, *global = NULL;
    tny_tool_ext_dirs(ctx, &project, &global);
    char *report = tny_tool_ext_report(ctx, true);
    tny_tool_ext_shutdown();
    yyjson_doc *doc = report ? jparse(report, strlen(report)) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *errors = jget(root, "errors");
    int rc = yyjson_arr_size(errors) ? 1 : 0;
    if (!root) rc = 1;
    else if (json) {
        puts(report);
    } else {
        printf("project: %s%s\n", project ? project : "(off)",
               project && !dir_exists(project) ? " (absent)" : "");
        printf("global:  %s%s\n", global ? global : "-",
               global && !dir_exists(global) ? " (absent)" : "");
        size_t idx, max;
        yyjson_val *t;
        yyjson_arr_foreach(jget(root, "tools"), idx, max, t) {
            int dl;
            const char *d = first_line(jget_str(t, "description"), &dl);
            printf("  %-28s %-7s %.*s\n", jget_str(t, "name"), jget_str(t, "scope"), dl, d);
        }
        yyjson_arr_foreach(jget(root, "warnings"), idx, max, t) print_problem(t, "warning");
        yyjson_arr_foreach(errors, idx, max, t) print_problem(t, "error");
        printf("%zu tools, %zu errors, %zu warnings\n", yyjson_arr_size(jget(root, "tools")),
               yyjson_arr_size(errors), yyjson_arr_size(jget(root, "warnings")));
    }
    yyjson_doc_free(doc);
    free(report);
    free(project);
    free(global);
    return rc;
}

static const char TEMPLATE[] =
    "\"\"\"%s: tools for tny (docs/extensions.md).\n"
    "\n"
    "Run it: tny %s \"hello\" --times 2   (or tools.%s(text=\"hello\") in code mode)\n"
    "\"\"\"\n"
    "\n"
    "import tny\n"
    "\n"
    "\n"
    "@tny.tool\n"
    "def %s(text: str, times: int = 1) -> str:\n"
    "    \"\"\"Repeat text.\n"
    "\n"
    "    Args:\n"
    "        text: What to repeat.\n"
    "        times: How many times.\n"
    "    \"\"\"\n"
    "    return \" \".join([text] * times)\n";

int cmd_tool_new(tny_ctx *ctx, int argc, char **argv) {
    const char *name = NULL;
    bool global = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--global") == 0) global = true;
        else if (!name) name = argv[i];
        else name = "";
    }
    bool valid = name && ((name[0] >= 'a' && name[0] <= 'z') ||
                          (name[0] >= 'A' && name[0] <= 'Z') || name[0] == '_');
    for (const char *c = name; valid && *c; c++)
        valid = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
                *c == '_';
    if (!valid || cli_is_command(name)) {
        fputs("tny: usage: tny tool new NAME [--global]  (NAME: a Python identifier that is not a "
              "tny command)\nExample: tny tool new call_advisor\n",
              stderr);
        return 1;
    }
    char *project = NULL, *home = NULL;
    tny_tool_ext_dirs(ctx, &project, &home);
    const char *dir = global ? home : project;
    if (!dir) dir = home; /* project tools off, or the workspace is ~ */
    char *file = NULL;
    buf_t b;
    buf_init(&b);
    if (dir) buf_appendf(&b, "%s/%s.py", dir, name);
    file = buf_detach(&b);
    int rc = 1;
    char *folder = dir ? path_join(dir, name) : NULL;
    if (!file || !folder) {
        fputs("tny: tool new: no extension directory\n", stderr);
    } else if (file_exists(file) || dir_exists(folder)) {
        fprintf(stderr, "tny: tool new: %s already exists\n", file_exists(file) ? file : folder);
    } else if (mkdir_p(dir) != 0) {
        fprintf(stderr, "tny: tool new: cannot create %s: %s\n", dir, strerror(errno));
    } else {
        buf_t src;
        buf_init(&src);
        buf_appendf(&src, TEMPLATE, name, name, name, name);
        if (!buf_oom(&src) && file_write_atomic(file, src.data, src.len) == 0) {
            printf("%s\n", file);
            rc = 0;
        } else {
            fprintf(stderr, "tny: tool new: cannot write %s\n", file);
        }
        buf_free(&src);
    }
    free(folder);
    free(file);
    free(project);
    free(home);
    return rc;
}

/* ---- tny mcp serve ------------------------------------------------------ */

static void mcp_send(yyjson_mut_doc *doc) {
    char *text = jwrite(doc);
    if (text) {
        fputs(text, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    free(text);
}

static void mcp_reply(yyjson_val *id, yyjson_mut_doc *doc, yyjson_mut_val *result) {
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(doc, root, "id", yyjson_val_mut_copy(doc, id));
    yyjson_mut_obj_add_val(doc, root, "result", result);
    yyjson_mut_doc_set_root(doc, root);
    mcp_send(doc);
}

static void mcp_error(yyjson_val *id, int code, const char *message) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(jallocator());
    if (!doc) return;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(doc, root, "id",
                           id ? yyjson_val_mut_copy(doc, id) : yyjson_mut_null(doc));
    yyjson_mut_val *err = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, err, "code", code);
    yyjson_mut_obj_add_strcpy(doc, err, "message", message);
    yyjson_mut_obj_add_val(doc, root, "error", err);
    yyjson_mut_doc_set_root(doc, root);
    mcp_send(doc);
    yyjson_mut_doc_free(doc);
}

/* MCP names allow [A-Za-z0-9_-]: dots become underscores. */
static void mcp_name(const char *name, char *out, size_t cap) {
    size_t j = 0;
    for (const char *c = name; *c && j + 1 < cap; c++) out[j++] = *c == '.' ? '_' : *c;
    out[j] = '\0';
}

static const char *turn_bound_name(const char *name) {
    static const char *const names[] = {
        "read_image", "vision", "read_tool_result", "search_transcript", "ask_user_question", NULL};
    for (size_t i = 0; names[i]; i++)
        if (strcmp(name, names[i]) == 0) return names[i];
    return NULL;
}

/* Served tools: extension tools (dotted, or listed by the extension report),
 * plus every native tool with --all. */
static bool served(yyjson_val *ext_names, const char *name, bool all) {
    if (turn_bound_name(name) || strcmp(name, "run_code") == 0) return false;
    if (all) return true;
    size_t idx, max;
    yyjson_val *t;
    yyjson_arr_foreach(ext_names, idx, max, t) {
        const char *n = jget_str(t, "name");
        if (n && strcmp(n, name) == 0) return true;
    }
    return false;
}

int cmd_mcp_serve(tny_ctx *ctx, int argc, char **argv) {
    bool all = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0) all = true;
        else {
            fprintf(stderr, "tny: usage: tny mcp serve [--all]\n");
            return 1;
        }
    }
    perm_engine *perm = perm_new(ctx);
    if (!perm) return 1;
    tny_tool_ext_set_stderr(STDERR_FILENO);
    tools_env env = {.ctx = ctx, .perm = perm};
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, stdin)) >= 0) {
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (!len) continue;
        yyjson_doc *req = jparse(line, (size_t)len);
        yyjson_val *root = req ? yyjson_doc_get_root(req) : NULL;
        if (!yyjson_is_obj(root)) {
            mcp_error(NULL, -32700, "parse error");
            yyjson_doc_free(req);
            continue;
        }
        const char *method = jget_str(root, "method");
        yyjson_val *id = jget(root, "id");
        yyjson_val *params = jget(root, "params");
        if (!method || !id) { /* notifications and stray responses */
            yyjson_doc_free(req);
            continue;
        }
        yyjson_mut_doc *out = yyjson_mut_doc_new(jallocator());
        if (strcmp(method, "initialize") == 0) {
            yyjson_mut_val *r = yyjson_mut_obj(out);
            const char *version = jget_str(params, "protocolVersion");
            yyjson_mut_obj_add_strcpy(out, r, "protocolVersion", version ? version : "2025-06-18");
            yyjson_mut_val *caps = yyjson_mut_obj(out);
            yyjson_mut_val *tools_cap = yyjson_mut_obj(out);
            yyjson_mut_obj_add_bool(out, tools_cap, "listChanged", false);
            yyjson_mut_obj_add_val(out, caps, "tools", tools_cap);
            yyjson_mut_obj_add_val(out, r, "capabilities", caps);
            yyjson_mut_val *info = yyjson_mut_obj(out);
            yyjson_mut_obj_add_str(out, info, "name", "tny");
            yyjson_mut_obj_add_str(out, info, "version", "1");
            yyjson_mut_obj_add_val(out, r, "serverInfo", info);
            mcp_reply(id, out, r);
        } else if (strcmp(method, "ping") == 0) {
            mcp_reply(id, out, yyjson_mut_obj(out));
        } else if (strcmp(method, "tools/list") == 0 || strcmp(method, "tools/call") == 0) {
            yyjson_doc *catalog = load_catalog(ctx);
            char *ext_json = tny_tool_ext_report(ctx, false);
            yyjson_doc *ext = ext_json ? jparse(ext_json, strlen(ext_json)) : NULL;
            free(ext_json);
            yyjson_val *ext_names = jget(ext ? yyjson_doc_get_root(ext) : NULL, "tools");
            if (strcmp(method, "tools/list") == 0) {
                yyjson_mut_val *list = yyjson_mut_arr(out);
                size_t idx, max;
                yyjson_val *item;
                yyjson_arr_foreach(catalog ? yyjson_doc_get_root(catalog) : NULL, idx, max, item) {
                    yyjson_val *fn = jget(item, "function");
                    const char *n = jget_str(fn, "name");
                    if (!n || !served(ext_names, n, all)) continue;
                    char mname[256];
                    mcp_name(n, mname, sizeof mname);
                    yyjson_mut_val *t = yyjson_mut_obj(out);
                    yyjson_mut_obj_add_strcpy(out, t, "name", mname);
                    const char *d = jget_str(fn, "description");
                    yyjson_mut_obj_add_strcpy(out, t, "description", d ? d : "");
                    yyjson_val *schema = jget(fn, "parameters");
                    yyjson_mut_obj_add_val(out, t, "inputSchema",
                                           schema ? yyjson_val_mut_copy(out, schema)
                                                  : yyjson_mut_obj(out));
                    yyjson_mut_arr_append(list, t);
                }
                yyjson_mut_val *r = yyjson_mut_obj(out);
                yyjson_mut_obj_add_val(out, r, "tools", list);
                mcp_reply(id, out, r);
            } else {
                const char *want = jget_str(params, "name");
                const char *found = NULL;
                size_t idx, max;
                yyjson_val *item;
                yyjson_arr_foreach(catalog ? yyjson_doc_get_root(catalog) : NULL, idx, max, item) {
                    const char *n = jget_str(jget(item, "function"), "name");
                    char mname[256];
                    if (!n || !want || !served(ext_names, n, all)) continue;
                    mcp_name(n, mname, sizeof mname);
                    if (strcmp(mname, want) == 0 || strcmp(n, want) == 0) {
                        found = n;
                        break;
                    }
                }
                if (!found) {
                    mcp_error(id, -32602, "unknown tool");
                } else {
                    yyjson_val *arguments = jget(params, "arguments");
                    char *args = arguments ? jwrite_val(arguments) : xstrdup("{}");
                    char *result = args ? tools_execute(&env, found, args) : NULL;
                    bool is_error = !result || str_starts(result, "error:");
                    yyjson_mut_val *r = yyjson_mut_obj(out);
                    yyjson_mut_val *content = yyjson_mut_arr(out);
                    yyjson_mut_val *text = yyjson_mut_obj(out);
                    yyjson_mut_obj_add_str(out, text, "type", "text");
                    yyjson_mut_obj_add_strcpy(out, text, "text",
                                              result ? result : "error: tool call failed");
                    yyjson_mut_arr_append(content, text);
                    yyjson_mut_obj_add_val(out, r, "content", content);
                    yyjson_mut_obj_add_bool(out, r, "isError", is_error);
                    mcp_reply(id, out, r);
                    free(result);
                    free(args);
                }
            }
            yyjson_doc_free(ext);
            yyjson_doc_free(catalog);
        } else {
            mcp_error(id, -32601, "method not found");
        }
        yyjson_mut_doc_free(out);
        yyjson_doc_free(req);
    }
    free(line);
    tools_discard_pending_images(&env);
    tny_tool_ext_shutdown();
    perm_free(perm);
    return 0;
}
