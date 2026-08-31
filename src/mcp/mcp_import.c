/* mcp_import.c — merge ~/.tny/mcp.json with opted-in foreign harness
 * profiles (docs/adr/0051). Foreign files are never opened unless
 * settings.json mcp.import_from names the source. */
#include "mcp/mcp_import.h"
#include "util/toml.h"
#include "util/util.h"

#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MCP_IMPORT_MAX_FILE (1024u * 1024u)

const char *mcp_source_name(mcp_source_id id) {
    switch (id) {
    case MCP_SRC_TNY: return "tny";
    case MCP_SRC_CODEX: return "codex";
    case MCP_SRC_CLAUDE: return "claude";
    case MCP_SRC_GROK: return "grok";
    case MCP_SRC_CURSOR: return "cursor-agent";
    }
    return "tny";
}

static void add_notice(mcp_catalog *cat, const char *fmt, ...) {
    if (!cat || cat->nnotices >= MCP_IMPORT_MAX_NOTICES) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    cat->notices[cat->nnotices++] = xstrdup(buf);
}

static void server_clear(mcp_imported_server *s) {
    if (!s) return;
    free(s->name);
    free(s->origin);
    free(s->scope);
    free(s->transport);
    secure_free(s->url);
    secure_free(s->raw);
    free(s->skip_reason);
    free(s->cwd);
    for (int i = 0; i < s->argc; i++) secure_free(s->argv[i]);
    for (int i = 0; i < s->nenv; i++) {
        free(s->env[i].key);
        secure_free(s->env[i].value);
    }
    memset(s, 0, sizeof *s);
}

void mcp_catalog_free(mcp_catalog *cat) {
    if (!cat) return;
    for (int i = 0; i < cat->nservers; i++) server_clear(&cat->servers[i]);
    for (int i = 0; i < cat->nnotices; i++) free(cat->notices[i]);
    free(cat);
}

const mcp_imported_server *mcp_catalog_find(const mcp_catalog *cat, const char *name) {
    if (!cat || !name) return NULL;
    for (int i = 0; i < cat->nservers; i++) {
        if (cat->servers[i].name && strcmp(cat->servers[i].name, name) == 0)
            return &cat->servers[i];
    }
    return NULL;
}

static bool name_ok(const char *name) {
    if (!name || !*name) return false;
    size_t n = 0;
    for (const char *p = name; *p; p++, n++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.'))
            return false;
        if (n >= 63) return false;
    }
    return true;
}

static bool env_name_ok(const char *name) {
    if (!name || !*name ||
        (!(name[0] == '_') && !(name[0] >= 'A' && name[0] <= 'Z') &&
         !(name[0] >= 'a' && name[0] <= 'z')))
        return false;
    for (const char *p = name + 1; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '_'))
            return false;
    return true;
}

/* Current Claude, grok, and Cursor configs support ${VAR} and
 * ${VAR:-default}. Expand without logging either names or values. */
static char *expand_env(const char *input) {
    buf_t out;
    buf_init(&out);
    const char *p = input ? input : "";
    while (*p) {
        const char *open = strstr(p, "${");
        if (!open) {
            buf_appends(&out, p);
            break;
        }
        buf_append(&out, p, (size_t)(open - p));
        const char *close = strchr(open + 2, '}');
        if (!close) {
            buf_free(&out);
            return NULL;
        }
        const char *body = open + 2;
        const char *def = NULL;
        for (const char *q = body; q + 1 < close; q++)
            if (q[0] == ':' && q[1] == '-') {
                def = q;
                break;
            }
        size_t name_len = (size_t)((def ? def : close) - body);
        char *name = xstrndup(body, name_len);
        const char *value = env_name_ok(name) ? getenv(name) : NULL;
        free(name);
        if (!value || !*value) {
            if (!def) {
                buf_free(&out);
                return NULL;
            }
            buf_append(&out, def + 2, (size_t)(close - (def + 2)));
        } else {
            buf_appends(&out, value);
        }
        p = close + 1;
    }
    if (buf_oom(&out)) {
        buf_free(&out);
        return NULL;
    }
    return buf_detach(&out);
}

static bool copy_env(mcp_imported_server *s, yyjson_val *env, bool expand) {
    if (!env) return true;
    if (!yyjson_is_obj(env)) return false;
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(env, idx, max, k, v) {
        if (s->nenv >= MCP_IMPORT_MAX_ENV) return false;
        if (!yyjson_is_str(k) || !yyjson_is_str(v)) return false;
        const char *key = yyjson_get_str(k);
        if (!env_name_ok(key)) return false;
        s->env[s->nenv].key = xstrdup(key);
        s->env[s->nenv].value = expand ? expand_env(yyjson_get_str(v)) : xstrdup(yyjson_get_str(v));
        s->nenv++; /* count before the check so server_clear frees the key */
        if (!s->env[s->nenv - 1].value) return false;
    }
    return true;
}

static bool argv_add(mcp_imported_server *s, const char *value, bool expand) {
    if (s->argc >= MCP_IMPORT_MAX_ARGV - 1 || !value) return false;
    s->argv[s->argc] = expand ? expand_env(value) : xstrdup(value);
    if (!s->argv[s->argc]) return false;
    s->argc++;
    return true;
}

static bool argv_from_command_args(mcp_imported_server *s, yyjson_val *conf, bool expand) {
    yyjson_val *cmd = jget(conf, "command");
    if (!cmd) return false;
    s->argc = 0;
    if (yyjson_is_arr(cmd)) {
        size_t idx, max;
        yyjson_val *v;
        yyjson_arr_foreach(cmd, idx, max, v) {
            if (!yyjson_is_str(v) || !argv_add(s, yyjson_get_str(v), expand)) {
                if (expand) return false;
                break; /* preserve native profile's historical prefix behavior */
            }
        }
        return s->argc > 0;
    }
    if (!yyjson_is_str(cmd) || !yyjson_get_str(cmd) || !yyjson_get_str(cmd)[0]) return false;
    if (!argv_add(s, yyjson_get_str(cmd), expand)) return false;
    yyjson_val *args = jget(conf, "args");
    if (args && !yyjson_is_arr(args)) return false;
    if (args && yyjson_is_arr(args)) {
        size_t idx, max;
        yyjson_val *v;
        yyjson_arr_foreach(args, idx, max, v) {
            if (!yyjson_is_str(v) || !argv_add(s, yyjson_get_str(v), expand)) return false;
        }
    }
    return s->argc > 0;
}

static const char *transport_of(yyjson_val *conf) {
    const char *type = jget_str(conf, "type");
    if (type && *type) return type;
    if (jget_str(conf, "url")) return "http";
    return "stdio";
}

bool mcp_transport_supported(const char *type) { return type && strcmp(type, "stdio") == 0; }

static void add_from_conf(mcp_catalog *cat, const char *name, mcp_source_id src, const char *scope,
                          yyjson_val *conf) {
    if (!cat || !conf || !yyjson_is_obj(conf)) return;
    if (src != MCP_SRC_TNY && !name_ok(name)) {
        add_notice(cat, "skipped unnamed or invalid MCP server from %s", mcp_source_name(src));
        return;
    }
    if (mcp_catalog_find(cat, name)) {
        if (src != MCP_SRC_TNY)
            add_notice(cat, "skipped '%s' from %s (name already taken)", name,
                       mcp_source_name(src));
        return;
    }
    if (cat->nservers >= MCP_IMPORT_MAX_SERVERS) {
        add_notice(cat, "skipped '%s' from %s (catalog full)", name, mcp_source_name(src));
        return;
    }
    mcp_imported_server *s = &cat->servers[cat->nservers];
    memset(s, 0, sizeof *s);
    s->name = xstrdup(name);
    s->source = src;
    s->origin = xstrdup(mcp_source_name(src));
    s->scope = xstrdup(scope);
    const char *type = transport_of(conf);
    const char *normalized =
        strcmp(type, "streamable-http") == 0 || strcmp(type, "streamable_http") == 0 ? "http"
                                                                                     : type;
    s->transport = xstrdup(normalized);
    const char *url = src == MCP_SRC_TNY ? NULL : jget_str(conf, "url");
    if (url) {
        s->url = src == MCP_SRC_TNY ? xstrdup(url) : expand_env(url);
        if (!s->url) {
            add_notice(cat, "skipped '%s' from %s (invalid or unresolved url)", name,
                       mcp_source_name(src));
            server_clear(s);
            return;
        }
    }
    if (src != MCP_SRC_TNY && !jget_bool(conf, "enabled", true)) {
        s->skipped = true;
        s->skip_reason = xstrdup("disabled in source config");
        add_notice(cat, "skipped '%s' from %s (disabled in source config)", name,
                   mcp_source_name(src));
        cat->nservers++;
        return;
    }
    if (src == MCP_SRC_TNY && strcmp(normalized, "http") == 0) {
        /* Native profile HTTP entries (docs/adr/0051) keep their full JSON:
         * the conn layer re-parses headers/bearer config at open time so
         * resolved secrets never sit in the long-lived catalog. */
        s->raw = jwrite_val(conf);
        if (!s->raw) {
            server_clear(s);
            return;
        }
        cat->nservers++;
        return;
    }
    if (!mcp_transport_supported(normalized)) {
        s->skipped = true;
        s->skip_reason = xstrdup("unsupported transport");
        add_notice(cat, "skipped '%s' from %s: unsupported transport (%s)", name,
                   mcp_source_name(src), normalized);
        cat->nservers++;
        return;
    }
    /* The remote transport branch consumes url once its capability seam says
     * the normalized transport is supported. This branch never implements
     * HTTP itself. */
    if (strcmp(normalized, "stdio") != 0) {
        cat->nservers++;
        return;
    }
    bool foreign = src != MCP_SRC_TNY;
    if (!foreign && !yyjson_is_arr(jget(conf, "command"))) {
        server_clear(s);
        return;
    }
    if (!argv_from_command_args(s, conf, foreign)) {
        add_notice(cat, "skipped '%s' from %s (invalid or unresolved stdio server)", name,
                   mcp_source_name(src));
        server_clear(s);
        return;
    }
    if (foreign && !copy_env(s, jget(conf, "env"), true)) {
        add_notice(cat, "skipped '%s' from %s (invalid or unresolved environment)", name,
                   mcp_source_name(src));
        server_clear(s);
        return;
    }
    const char *cwd = foreign ? jget_str(conf, "cwd") : NULL;
    if (cwd) {
        s->cwd = expand_env(cwd);
        if (!s->cwd) {
            add_notice(cat, "skipped '%s' from %s (invalid or unresolved cwd)", name,
                       mcp_source_name(src));
            server_clear(s);
            return;
        }
    }
    cat->nservers++;
}

static void add_from_map(mcp_catalog *cat, yyjson_val *map, mcp_source_id src, const char *scope) {
    if (!map || !yyjson_is_obj(map)) return;
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(map, idx, max, k, v) {
        const char *name = yyjson_get_str(k);
        add_from_conf(cat, name, src, scope, v);
    }
}

static void load_tny(mcp_catalog *cat, struct tny_ctx *ctx) {
    char *file = path_join(ctx->tny_dir, "mcp.json");
    yyjson_doc *doc = jparse_file(file);
    free(file);
    if (!doc) return;
    add_from_map(cat, jget(yyjson_doc_get_root(doc), "servers"), MCP_SRC_TNY, "user");
    yyjson_doc_free(doc);
}

static char *home_join2(const char *a, const char *b) {
    char *home = path_home();
    if (!home) return NULL;
    char *mid = path_join(home, a);
    free(home);
    if (!b) return mid;
    char *out = path_join(mid, b);
    free(mid);
    return out;
}

/* Foreign paths are untrusted. Reject devices/FIFOs and oversized files so
 * an enabled source cannot block startup or consume unbounded memory. */
static char *read_foreign_file(const char *path, size_t *len) {
    *len = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > MCP_IMPORT_MAX_FILE) {
        close(fd);
        return NULL;
    }
    *len = (size_t)st.st_size;
    char *body = malloc(*len + 1);
    if (!body) {
        close(fd);
        return NULL;
    }
    size_t off = 0;
    while (off < *len) {
        ssize_t n = read(fd, body + off, *len - off);
        if (n <= 0) {
            free(body);
            close(fd);
            return NULL;
        }
        off += (size_t)n;
    }
    close(fd);
    body[*len] = '\0';
    return body;
}

static yyjson_doc *read_foreign_json(const char *path) {
    size_t len = 0;
    char *body = read_foreign_file(path, &len);
    if (!body) return NULL;
    yyjson_doc *doc = yyjson_read(body, len, 0);
    free(body);
    return doc;
}

static void catalog_clear(mcp_catalog *cat) {
    for (int i = 0; i < cat->nservers; i++) server_clear(&cat->servers[i]);
    for (int i = 0; i < cat->nnotices; i++) free(cat->notices[i]);
    memset(cat, 0, sizeof *cat);
}

/* Move one successfully parsed source into the merged catalog. The caller
 * loads higher-precedence files first, so the first same-name entry wins. */
static void merge_catalog(mcp_catalog *dst, mcp_catalog *src) {
    for (int i = 0; i < src->nservers; i++) {
        mcp_imported_server *server = &src->servers[i];
        if (mcp_catalog_find(dst, server->name)) {
            add_notice(dst, "skipped '%s' from %s (name already taken)", server->name,
                       server->origin);
            server_clear(server);
            continue;
        }
        if (dst->nservers >= MCP_IMPORT_MAX_SERVERS) {
            add_notice(dst, "skipped '%s' from %s (catalog full)", server->name, server->origin);
            server_clear(server);
            continue;
        }
        dst->servers[dst->nservers++] = *server;
        memset(server, 0, sizeof *server);
    }
    for (int i = 0; i < src->nnotices; i++) {
        if (src->notices[i]) {
            add_notice(dst, "%s", src->notices[i]);
            free(src->notices[i]);
            src->notices[i] = NULL;
        }
    }
    src->nservers = 0;
    src->nnotices = 0;
}

static bool load_json_map(mcp_catalog *cat, const char *path, mcp_source_id src,
                          const char *scope) {
    yyjson_doc *doc = read_foreign_json(path);
    if (!doc) return false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *map = yyjson_is_obj(root) ? jget(root, "mcpServers") : NULL;
    bool ok = yyjson_is_obj(root) && (!map || yyjson_is_obj(map));
    if (ok) add_from_map(cat, map, src, scope);
    yyjson_doc_free(doc);
    return ok;
}

static bool load_toml_map(mcp_catalog *cat, const char *path, mcp_source_id src,
                          const char *scope) {
    size_t len = 0;
    char *body = read_foreign_file(path, &len);
    if (!body) return false;
    yyjson_doc *doc = toml_parse_subset(body, len);
    free(body);
    if (!doc) return false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *map = yyjson_is_obj(root) ? jget(root, "mcp_servers") : NULL;
    bool ok = yyjson_is_obj(root) && (!map || yyjson_is_obj(map));
    if (ok) add_from_map(cat, map, src, scope);
    yyjson_doc_free(doc);
    return ok;
}

static void source_failed(mcp_catalog *cat, mcp_catalog *pending, mcp_source_id src,
                          const char *why) {
    catalog_clear(pending);
    add_notice(cat, "%s MCP import disabled: %s", mcp_source_name(src), why);
}

static void load_claude(mcp_catalog *cat, struct tny_ctx *ctx) {
    char *user = home_join2(".claude.json", NULL);
    char *project = path_join(ctx->cwd, ".mcp.json");
    bool user_exists = user && file_exists(user);
    bool project_exists = project && file_exists(project);
    if (!user_exists && !project_exists) {
        add_notice(cat, "claude MCP import: no config file found");
        free(user);
        free(project);
        return;
    }
    mcp_catalog pending = {0};
    yyjson_doc *user_doc = NULL;
    if (user_exists) {
        user_doc = read_foreign_json(user);
        if (!user_doc || !yyjson_is_obj(yyjson_doc_get_root(user_doc))) {
            yyjson_doc_free(user_doc);
            source_failed(cat, &pending, MCP_SRC_CLAUDE, "malformed ~/.claude.json");
            free(user);
            free(project);
            return;
        }
        yyjson_val *root = yyjson_doc_get_root(user_doc);
        yyjson_val *local = jget(jget(jget(root, "projects"), ctx->cwd), "mcpServers");
        if (local && !yyjson_is_obj(local)) {
            yyjson_doc_free(user_doc);
            source_failed(cat, &pending, MCP_SRC_CLAUDE,
                          "malformed project entry in ~/.claude.json");
            free(user);
            free(project);
            return;
        }
        add_from_map(&pending, local, MCP_SRC_CLAUDE, "project"); /* local scope wins */
    }
    if (project_exists) {
        mcp_catalog project_cat = {0};
        if (!load_json_map(&project_cat, project, MCP_SRC_CLAUDE, "project")) {
            yyjson_doc_free(user_doc);
            catalog_clear(&project_cat);
            source_failed(cat, &pending, MCP_SRC_CLAUDE, "malformed project .mcp.json");
            free(user);
            free(project);
            return;
        }
        merge_catalog(&pending, &project_cat); /* project scope is second */
    }
    if (user_doc) {
        yyjson_val *map = jget(yyjson_doc_get_root(user_doc), "mcpServers");
        if (map && !yyjson_is_obj(map)) {
            yyjson_doc_free(user_doc);
            source_failed(cat, &pending, MCP_SRC_CLAUDE,
                          "malformed user mcpServers in ~/.claude.json");
            free(user);
            free(project);
            return;
        }
        mcp_catalog user_cat = {0};
        add_from_map(&user_cat, map, MCP_SRC_CLAUDE, "user");
        merge_catalog(&pending, &user_cat);
        yyjson_doc_free(user_doc);
    }
    merge_catalog(cat, &pending);
    free(user);
    free(project);
}

static void load_cursor(mcp_catalog *cat, struct tny_ctx *ctx) {
    char *user = home_join2(".cursor", "mcp.json");
    char *project = path_join(ctx->cwd, ".cursor/mcp.json");
    const char *paths[2] = {project, user}; /* project wins */
    bool found = false;
    mcp_catalog pending = {0};
    for (int i = 0; i < 2; i++) {
        if (!paths[i] || !file_exists(paths[i])) continue;
        found = true;
        mcp_catalog one = {0};
        if (!load_json_map(&one, paths[i], MCP_SRC_CURSOR, i == 0 ? "project" : "user")) {
            catalog_clear(&one);
            source_failed(cat, &pending, MCP_SRC_CURSOR, "malformed config");
            free(user);
            free(project);
            return;
        }
        merge_catalog(&pending, &one);
    }
    if (!found) add_notice(cat, "cursor-agent MCP import: no config file found");
    else merge_catalog(cat, &pending);
    free(user);
    free(project);
}

static void load_grok(mcp_catalog *cat, struct tny_ctx *ctx) {
    char *user = home_join2(".grok", "config.toml");
    bool found = false;
    mcp_catalog pending = {0};
    /* Load deepest first so merge_catalog preserves Grok's deeper-wins
     * overlay semantics while walking back to the git root. */
    char *project_root = xstrdup(ctx->cwd);
    char *scan = xstrdup(ctx->cwd);
    while (scan && *scan) {
        char *git = path_join(scan, ".git");
        bool found_root = file_exists(git) || dir_exists(git);
        free(git);
        if (found_root) {
            free(project_root);
            project_root = xstrdup(scan);
            break;
        }
        if (strcmp(scan, "/") == 0) break;
        char *slash = strrchr(scan, '/');
        if (!slash) break;
        if (slash == scan) scan[1] = '\0';
        else *slash = '\0';
    }
    free(scan);
    char *dir = xstrdup(ctx->cwd);
    for (int depth = 0; dir && depth < 128; depth++) {
        char *project = path_join(dir, ".grok/config.toml");
        bool exists = file_exists(project);
        if (exists) found = true;
        mcp_catalog one = {0};
        if (exists && !load_toml_map(&one, project, MCP_SRC_GROK, "project")) {
            catalog_clear(&one);
            source_failed(cat, &pending, MCP_SRC_GROK, "malformed config.toml");
            free(user);
            free(project);
            free(dir);
            free(project_root);
            return;
        }
        if (exists) merge_catalog(&pending, &one);
        free(project);
        bool stop = strcmp(dir, project_root) == 0;
        if (stop) break;
        char *slash = strrchr(dir, '/');
        if (!slash) break;
        if (slash == dir) dir[1] = '\0';
        else *slash = '\0';
    }
    free(dir);
    free(project_root);
    if (user && file_exists(user)) {
        found = true;
        mcp_catalog one = {0};
        if (!load_toml_map(&one, user, MCP_SRC_GROK, "user")) {
            catalog_clear(&one);
            source_failed(cat, &pending, MCP_SRC_GROK, "malformed config.toml");
            free(user);
            return;
        }
        merge_catalog(&pending, &one);
    }
    if (!found) add_notice(cat, "grok MCP import: no config file found");
    else merge_catalog(cat, &pending);
    free(user);
}

static void load_codex(mcp_catalog *cat) {
    const char *env = getenv("CODEX_HOME");
    char *path = env && *env ? path_join(env, "config.toml") : home_join2(".codex", "config.toml");
    if (!path || !file_exists(path)) {
        add_notice(cat, "codex MCP import: no config file found");
        free(path);
        return;
    }
    mcp_catalog pending = {0};
    if (!load_toml_map(&pending, path, MCP_SRC_CODEX, "user"))
        source_failed(cat, &pending, MCP_SRC_CODEX, "malformed config.toml");
    else merge_catalog(cat, &pending);
    free(path);
}

static void load_source(mcp_catalog *cat, struct tny_ctx *ctx, mcp_source_id src) {
    switch (src) {
    case MCP_SRC_CODEX: load_codex(cat); break;
    case MCP_SRC_CLAUDE: load_claude(cat, ctx); break;
    case MCP_SRC_GROK: load_grok(cat, ctx); break;
    case MCP_SRC_CURSOR: load_cursor(cat, ctx); break;
    case MCP_SRC_TNY: break;
    }
}

mcp_catalog *mcp_catalog_load(struct tny_ctx *ctx) {
    mcp_catalog *cat = calloc(1, sizeof *cat);
    if (!cat) return NULL;
    cat->loaded = true;
    if (!ctx || ctx->mcp_disabled || ctx->library_mode) return cat;
    load_tny(cat, ctx);
    /* Off by default: n_mcp_import_sources is zero unless settings named a
     * source. No foreign path is built or opened before that. */
    for (int i = 0; i < ctx->n_mcp_import_sources; i++) {
        unsigned bit = ctx->mcp_import_order[i];
        if (bit == TNY_MCP_IMPORT_CODEX) load_source(cat, ctx, MCP_SRC_CODEX);
        else if (bit == TNY_MCP_IMPORT_CLAUDE) load_source(cat, ctx, MCP_SRC_CLAUDE);
        else if (bit == TNY_MCP_IMPORT_GROK) load_source(cat, ctx, MCP_SRC_GROK);
        else if (bit == TNY_MCP_IMPORT_CURSOR) load_source(cat, ctx, MCP_SRC_CURSOR);
    }
    if (ctx->n_mcp_import_sources && !ctx->mcp_import_warned) {
        for (int i = 0; i < cat->nnotices; i++)
            fprintf(stderr, "tny: warning: %s\n", cat->notices[i]);
        ctx->mcp_import_warned = true;
    }
    return cat;
}
