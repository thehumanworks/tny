/* tools_ssh.c — workspace tools executed on the --ssh host (docs/adr/0022).
 *
 * Same tool names, same result shapes as tools_fs.c / tools_shell.c, but every
 * byte of file or process I/O happens remotely through ssh_run(). The remote
 * side needs POSIX sh + coreutils + grep/find only. Content-bearing arguments
 * (write_file, edit_file) travel on stdin, never inside the command line. */
#include "core/tools.h"
#include "core/edit.h"
#include "core/ssh.h"
#include "core/image.h"
#include "util/util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define R_MAX_OUT  (512u * 1024u)
#define R_MAX_FILE (8u * 1024u * 1024u)
#define PRUNE                                                             \
    "\\( -name .git -o -name node_modules -o -name build -o -name target" \
    " -o -name dist -o -name __pycache__ \\) -prune -o "

/* Remote path: absolute and ~ pass through (sh expands ~ only unquoted, so
 * rewrite ~/x to $HOME/x); relative joins ctx->ssh_cwd. malloc'd. */
static char *rpath(tools_env *env, const char *p, char **err) {
    *err = NULL;
    if (!p || !*p) {
        *err = tool_err("missing path");
        return NULL;
    }
    buf_t b;
    buf_init(&b);
    if (p[0] == '/') buf_appends(&b, p);
    else if (p[0] == '~' && (p[1] == '/' || p[1] == '\0')) {
        buf_appends(&b, "$HOME"); /* expanded by the remote sh; see q_path */
        buf_appends(&b, p + 1);
    } else {
        buf_appends(&b, env->ctx->ssh_cwd);
        if (strcmp(p, ".") != 0) {
            buf_appends(&b, "/");
            buf_appends(&b, p);
        }
    }
    return buf_detach(&b);
}

/* Quote a path for the remote sh, letting a leading $HOME expand. */
static void q_path(buf_t *b, const char *path) {
    if (strncmp(path, "$HOME", 5) == 0) {
        buf_appends(b, "\"$HOME\"");
        if (path[5]) ssh_shell_quote(b, path + 5);
    } else ssh_shell_quote(b, path);
}

static int run(tools_env *env, const char *script, const char *in, size_t inlen, int timeout_s,
               buf_t *out, bool *truncated, bool *timed_out) {
    return ssh_run(env->ctx, script, in, inlen, timeout_s, R_MAX_OUT, out, truncated, timed_out);
}

static void chomp(buf_t *b) {
    while (b->len && (b->data[b->len - 1] == '\n' || b->data[b->len - 1] == '\r'))
        b->data[--b->len] = 0;
}

static char *bounded_or_empty(tools_env *env, buf_t *out, const char *empty) {
    if (!out->len) buf_appends(out, empty);
    char *res = tool_bound_result(env, out->data, out->len);
    buf_free(out);
    return res;
}

static char *r_list_files(tools_env *env, yyjson_val *args) {
    const char *p = jget_str(args, "path");
    char *err;
    char *path = rpath(env, p && *p ? p : ".", &err);
    if (!path) return err;
    buf_t s, out;
    buf_init(&s);
    buf_init(&out);
    /* a pipeline reports head's status, so test the directory first */
    buf_appends(&s, "p=");
    q_path(&s, path);
    buf_appends(&s, " && { [ -d \"$p\" ] || { echo \"not a directory\" >&2; exit 1; }; }"
                    " && ls -1Ap -- \"$p\" | head -n 2000");
    bool tr, to;
    int rc = run(env, s.data, NULL, 0, 60, &out, &tr, &to);
    buf_free(&s);
    if (rc != 0) {
        chomp(&out);
        char *e = tool_err("cannot open %s: %s", path, out.data ? out.data : "");
        buf_free(&out);
        free(path);
        return e;
    }
    free(path);
    return bounded_or_empty(env, &out, "(empty)");
}

static char *r_glob_files(tools_env *env, yyjson_val *args) {
    const char *pat = jget_str(args, "pattern");
    if (!pat) return tool_err("missing pattern");
    const char *p = jget_str(args, "path");
    char *err;
    char *path = rpath(env, p && *p ? p : ".", &err);
    if (!path) return err;
    /* normalize ** to * (find -path '*' crosses '/') and anchor at ./ */
    buf_t np;
    buf_init(&np);
    buf_appends(&np, "./");
    for (const char *q = pat; *q; q++) {
        if (*q == '*' && q[1] == '*') {
            buf_appends(&np, "*");
            q++;
        } else buf_append(&np, q, 1);
    }
    buf_t s, out;
    buf_init(&s);
    buf_init(&out);
    buf_appends(&s, "cd ");
    q_path(&s, path);
    buf_appends(&s, " && find . " PRUNE "-type f -path ");
    ssh_shell_quote(&s, np.data);
    buf_appends(&s, " -print | sed 's|^\\./||' | head -n 1000");
    buf_free(&np);
    bool tr, to;
    run(env, s.data, NULL, 0, 120, &out, &tr, &to);
    buf_free(&s);
    free(path);
    return bounded_or_empty(env, &out, "(no matches)");
}

static char *r_grep_files(tools_env *env, yyjson_val *args) {
    const char *pat = jget_str(args, "pattern");
    if (!pat) return tool_err("missing pattern");
    const char *p = jget_str(args, "path");
    char *err;
    char *path = rpath(env, p && *p ? p : ".", &err);
    if (!path) return err;
    buf_t s, out;
    buf_init(&s);
    buf_init(&out);
    /* -F: substring semantics like the local tool; -I skips binaries */
    buf_appends(&s, "cd ");
    q_path(&s, path);
    buf_appends(&s, " 2>/dev/null && { find . " PRUNE "-type f -print0 | xargs -0 grep -nIF");
    if (jget_bool(args, "case_insensitive", false)) buf_appends(&s, "i");
    buf_appends(&s, " -e ");
    ssh_shell_quote(&s, pat);
    buf_appends(&s,
                " -- 2>/dev/null | sed 's|^\\./||' | cut -c1-400 | head -n 500; } || grep -nIF");
    if (jget_bool(args, "case_insensitive", false)) buf_appends(&s, "i");
    buf_appends(&s, " -e ");
    ssh_shell_quote(&s, pat);
    buf_appends(&s, " -- ");
    q_path(&s, path);
    buf_appends(&s, " 2>/dev/null | cut -c1-400 | head -n 500");
    bool tr, to;
    run(env, s.data, NULL, 0, 120, &out, &tr, &to);
    buf_free(&s);
    free(path);
    return bounded_or_empty(env, &out, "(no matches)");
}

/* cat a remote file; 0 ok with bytes in out, else tool error returned. */
static char *r_cat(tools_env *env, const char *path, buf_t *out) {
    buf_t s;
    buf_init(&s);
    buf_appends(&s, "cat -- ");
    q_path(&s, path);
    bool tr, to;
    int rc = ssh_run(env->ctx, s.data, NULL, 0, 120, R_MAX_FILE, out, &tr, &to);
    buf_free(&s);
    if (rc != 0) {
        chomp(out);
        char *e = tool_err("cannot read %s: %s", path, out->data ? out->data : "");
        buf_free(out);
        return e;
    }
    if (tr) {
        buf_free(out);
        return tool_err("%s is larger than %u bytes; read it with terminal + sed/head", path,
                        R_MAX_FILE);
    }
    return NULL;
}

static char *r_read_file(tools_env *env, yyjson_val *args) {
    char *err;
    char *path = rpath(env, jget_str(args, "path"), &err);
    if (!path) return err;
    buf_t data;
    buf_init(&data);
    char *e = r_cat(env, path, &data);
    if (e) {
        free(path);
        return e;
    }
    const char *mime = image_mime((const uint8_t *)data.data, data.len);
    if (mime) {
        e = tool_err("%s is %s; use read_image to view it", path, mime);
        buf_free(&data);
        free(path);
        return e;
    }
    free(path);
    int64_t off = jget_int(args, "offset", 0);
    int64_t lim = jget_int(args, "limit", 0);
    if (off <= 0 && lim <= 0) return bounded_or_empty(env, &data, "");
    buf_t out;
    buf_init(&out);
    size_t start = 0;
    int line = 1, emitted = 0;
    for (size_t i = 0; i <= data.len; i++) {
        if (i == data.len || data.data[i] == '\n') {
            if (line >= (off > 0 ? off : 1)) {
                buf_append(&out, data.data + start, i - start);
                buf_appends(&out, "\n");
                emitted++;
                if (lim > 0 && emitted >= lim) break;
            }
            start = i + 1;
            line++;
        }
    }
    buf_free(&data);
    return bounded_or_empty(env, &out, "");
}

/* write bytes to a remote path (parents created), atomically via rename. */
static int r_put(tools_env *env, const char *path, const char *data, size_t len, buf_t *msg) {
    buf_t s;
    buf_init(&s);
    buf_appends(&s, "p=");
    q_path(&s, path);
    buf_appends(&s, " && d=$(dirname -- \"$p\") && mkdir -p -- \"$d\" && t=\"$p.tny.$$\""
                    " && cat > \"$t\" && mv -f -- \"$t\" \"$p\"");
    bool tr, to;
    int rc = run(env, s.data, data, len, 120, msg, &tr, &to);
    buf_free(&s);
    return rc;
}

static char *r_write_file(tools_env *env, yyjson_val *args) {
    char *err;
    char *path = rpath(env, jget_str(args, "path"), &err);
    if (!path) return err;
    const char *content = jget_str(args, "content");
    if (!content) {
        free(path);
        return tool_err("missing content");
    }
    buf_t msg, out;
    buf_init(&msg);
    buf_init(&out);
    int rc = r_put(env, path, content, strlen(content), &msg);
    chomp(&msg);
    if (rc == 0) buf_appendf(&out, "wrote %zu bytes to %s", strlen(content), path);
    else buf_appendf(&out, "error: write to %s failed: %s", path, msg.data ? msg.data : "");
    buf_free(&msg);
    free(path);
    return buf_detach(&out);
}

static char *r_edit_file(tools_env *env, yyjson_val *args) {
    char *err;
    char *path = rpath(env, jget_str(args, "path"), &err);
    if (!path) return err;
    const char *olds = jget_str(args, "old_string");
    const char *news = jget_str(args, "new_string");
    bool all = jget_bool(args, "replace_all", false);
    if (!olds || !news || !*olds) {
        free(path);
        return tool_err("missing old_string/new_string");
    }
    buf_t data;
    buf_init(&data);
    char *e = r_cat(env, path, &data);
    if (e) {
        free(path);
        return e;
    }
    if (memchr(data.data, 0, data.len)) {
        buf_free(&data);
        e = tool_err("%s is binary", path);
        free(path);
        return e;
    }
    size_t ol = strlen(olds);
    int count = 0;
    for (char *p = data.data; (p = strstr(p, olds)); p += ol) count++;
    if (count == 0 || (count > 1 && !all)) {
        e = count == 0
                ? tool_err("old_string not found in %s", path)
                : tool_err("old_string occurs %d times in %s; pass replace_all or a longer match",
                           count, path);
        buf_free(&data);
        free(path);
        return e;
    }
    buf_t out;
    buf_init(&out);
    char *p = data.data;
    for (;;) {
        char *hit = strstr(p, olds);
        if (!hit) {
            buf_appends(&out, p);
            break;
        }
        buf_append(&out, p, (size_t)(hit - p));
        buf_appends(&out, news);
        p = hit + ol;
        if (!all) {
            buf_appends(&out, p);
            break;
        }
    }
    buf_free(&data);
    buf_t msg, res;
    buf_init(&msg);
    buf_init(&res);
    int rc = r_put(env, path, out.data, out.len, &msg);
    buf_free(&out);
    chomp(&msg);
    if (rc == 0)
        buf_appendf(&res, "replaced %d occurrence%s in %s", all ? count : 1,
                    (all && count > 1) ? "s" : "", path);
    else buf_appendf(&res, "error: write to %s failed: %s", path, msg.data ? msg.data : "");
    buf_free(&msg);
    free(path);
    return buf_detach(&res);
}

/* The intercepted `tny edit` on the --ssh host (docs/adr/0063). The remote
 * side stays POSIX sh: cat the file out, run the shared exact-match engine on
 * a local staging copy so the model gets the same match policy and nearest
 * context as it would locally, then stream the result back into a temp file
 * that is renamed into place. There is no remote undo record, exactly as for
 * the remote edit_file tool. */
tny_edit_status tool_ssh_edit_exact(tools_env *env, const char *path, const char *old_text,
                                    const char *new_text, bool replace_all, tny_edit_result *result,
                                    char **err_out) {
    *err_out = NULL;
    memset(result, 0, sizeof *result);
    char *rerr = NULL;
    char *remote = rpath(env, path, &rerr);
    if (!remote) {
        *err_out = rerr;
        return TNY_EDIT_READ_ERROR;
    }
    buf_t data;
    buf_init(&data);
    char *cat_error = r_cat(env, remote, &data);
    if (cat_error) {
        *err_out = xstrdup(str_starts(cat_error, "error: ") ? cat_error + 7 : cat_error);
        free(cat_error);
        free(remote);
        return TNY_EDIT_READ_ERROR;
    }

    const char *tmpdir = getenv("TMPDIR");
    char staged[4096];
    snprintf(staged, sizeof staged, "%s/tny-ssh-edit-XXXXXX", tmpdir && *tmpdir ? tmpdir : "/tmp");
    int fd = mkstemp(staged);
    bool wrote = fd >= 0 && (data.len == 0 || write(fd, data.data, data.len) == (ssize_t)data.len);
    if (fd >= 0) close(fd);
    buf_free(&data);
    if (!wrote) {
        if (fd >= 0) unlink(staged);
        *err_out = xstrdup("cannot stage the remote file locally");
        free(remote);
        return TNY_EDIT_READ_ERROR;
    }

    tny_edit_status status =
        tny_edit_file_exact(staged, old_text, new_text, replace_all, NULL, result);
    if (status == TNY_EDIT_OK) {
        size_t len = 0;
        char *edited = file_slurp(staged, &len);
        buf_t msg;
        buf_init(&msg);
        int rc = edited ? r_put(env, remote, edited, len, &msg) : -1;
        free(edited);
        chomp(&msg);
        if (rc != 0) {
            buf_t why;
            buf_init(&why);
            buf_appendf(&why, "cannot write %s: %s", remote, msg.data ? msg.data : "");
            *err_out = buf_detach(&why);
            status = TNY_EDIT_WRITE_ERROR;
        }
        buf_free(&msg);
    }
    unlink(staged);
    free(remote);
    return status;
}

static char *r_simple_path_op(tools_env *env, yyjson_val *args, const char *op) {
    char *err;
    char *path = rpath(env, jget_str(args, "path"), &err);
    if (!path) return err;
    buf_t s, out, res;
    buf_init(&s);
    buf_init(&out);
    buf_init(&res);
    const char *okfmt, *errfmt;
    if (strcmp(op, "delete") == 0) {
        buf_appends(&s, "p=");
        q_path(&s, path);
        buf_appends(&s, " && { rm -f -- \"$p\" || rmdir -- \"$p\"; }");
        okfmt = "deleted %s";
        errfmt = "error: cannot delete %s";
    } else if (strcmp(op, "mkdir") == 0) {
        buf_appends(&s, "mkdir -p -- ");
        q_path(&s, path);
        okfmt = "created %s";
        errfmt = "error: cannot create %s";
    } else {
        buf_appends(&s, "p=");
        q_path(&s, path);
        buf_appends(&s, " && if [ -d \"$p\" ]; then echo directory; else echo file; fi"
                        " && wc -c < \"$p\" 2>/dev/null; ls -ld -- \"$p\"");
        okfmt = NULL;
        errfmt = "error: cannot stat %s";
    }
    bool tr, to;
    int rc = run(env, s.data, NULL, 0, 60, &out, &tr, &to);
    buf_free(&s);
    chomp(&out);
    if (rc != 0) buf_appendf(&res, errfmt, path);
    else if (okfmt) buf_appendf(&res, okfmt, path);
    else {
        /* "directory|file\n<bytes>\n<ls -ld line>" → "%s: %s, %lld bytes, <ls>" */
        char *l1 = out.data, *l2 = l1 ? strchr(l1, '\n') : NULL, *l3 = NULL;
        if (l2) {
            *l2++ = 0;
            l3 = strchr(l2, '\n');
            if (l3) *l3++ = 0;
        }
        bool isdir = l1 && strcmp(l1, "directory") == 0;
        long long bytes = (l2 && !isdir) ? atoll(l2) : 0;
        buf_appendf(&res, "%s: %s, %lld bytes", path, isdir ? "directory" : "file", bytes);
        const char *ls = isdir ? l2 : l3;
        if (ls && *ls) buf_appendf(&res, ", %s", ls);
    }
    buf_free(&out);
    free(path);
    return buf_detach(&res);
}

static char *r_two_path_op(tools_env *env, yyjson_val *args, bool copy) {
    char *err;
    char *src = rpath(env, jget_str(args, "path"), &err);
    if (!src) return err;
    char *dst = rpath(env, jget_str(args, "new_path"), &err);
    if (!dst) {
        free(src);
        return err;
    }
    buf_t s, out, res;
    buf_init(&s);
    buf_init(&out);
    buf_init(&res);
    buf_appends(&s, "d=");
    q_path(&s, dst);
    buf_appends(&s, " && mkdir -p -- \"$(dirname -- \"$d\")\" && ");
    buf_appends(&s, copy ? "cp -R -- " : "mv -f -- ");
    q_path(&s, src);
    buf_appends(&s, " \"$d\"");
    bool tr, to;
    int rc = run(env, s.data, NULL, 0, 120, &out, &tr, &to);
    buf_free(&s);
    chomp(&out);
    const char *verb = copy ? "copied" : "renamed";
    if (rc == 0) buf_appendf(&res, "%s %s -> %s", verb, src, dst);
    else
        buf_appendf(&res, "error: %s %s -> %s failed: %s", copy ? "copy" : "rename", src, dst,
                    out.data ? out.data : "");
    buf_free(&out);
    free(src);
    free(dst);
    return buf_detach(&res);
}

/* semantic_search: same lexical idea as tools_fs — count term hits per file
 * with grep -c, rank the top 10. */
static char *r_semantic_search(tools_env *env, yyjson_val *args) {
    const char *q = jget_str(args, "query");
    if (!q) return tool_err("missing query");
    buf_t terms;
    buf_init(&terms);
    int nterms = 0;
    const char *p = q;
    while (*p && nterms < 8) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        size_t tl = (size_t)(p - start);
        if (tl >= 3 && tl < 63) {
            char t[64];
            memcpy(t, start, tl);
            t[tl] = 0;
            buf_appends(&terms, " -e ");
            ssh_shell_quote(&terms, t);
            nterms++;
        }
    }
    if (!nterms) {
        buf_free(&terms);
        return tool_err("query has no searchable terms");
    }
    buf_t s, out, res;
    buf_init(&s);
    buf_init(&out);
    buf_init(&res);
    buf_appends(&s, "find . " PRUNE "-type f -size -2000k -print0 | xargs -0 grep -ciI");
    buf_append(&s, terms.data, terms.len);
    buf_appends(
        &s, " -- 2>/dev/null | grep -v ':0$' | sort -t: -k2,2nr | head -n 10 | sed 's|^\\./||'");
    buf_free(&terms);
    bool tr, to;
    run(env, s.data, NULL, 0, 120, &out, &tr, &to);
    buf_free(&s);
    /* "rel:count" → "rel (score count)" */
    size_t start = 0;
    for (size_t i = 0; i <= out.len; i++) {
        if (i == out.len || out.data[i] == '\n') {
            if (i > start) {
                char *line = xstrndup(out.data + start, i - start);
                char *c = strrchr(line, ':');
                if (c) {
                    *c = 0;
                    buf_appendf(&res, "%s (score %s)\n", line, c + 1);
                }
                free(line);
            }
            start = i + 1;
        }
    }
    buf_free(&out);
    if (!res.len) buf_appends(&res, "(no relevant files found)");
    return buf_detach(&res);
}

static char *r_read_image(tools_env *env, yyjson_val *args) {
    char *err;
    char *path = rpath(env, jget_str(args, "path"), &err);
    if (!path) return err;
    buf_t data;
    buf_init(&data);
    char *e = r_cat(env, path, &data);
    if (e) {
        free(path);
        return e;
    }
    const char *mime = image_mime((const uint8_t *)data.data, data.len);
    if (!mime) {
        buf_free(&data);
        e = tool_err("%s is not a png/jpeg/gif/webp image", path);
        free(path);
        return e;
    }
    if (env->n_pending_images >= 8) {
        buf_free(&data);
        free(path);
        return tool_err("too many images in this step (max 8)");
    }
    /* Pixels have to be local for the provider upload: stage a copy under
     * the session dir (or $TMPDIR) and queue that path like the local tool. */
    char *dir = env->session ? path_join(env->session->dir, "ssh-images")
                             : xstrdup(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    mkdir_p(dir);
    char *id = gen_id();
    const char *ext = strstr(mime, "png")    ? "png"
                      : strstr(mime, "gif")  ? "gif"
                      : strstr(mime, "webp") ? "webp"
                                             : "jpg";
    buf_t local;
    buf_init(&local);
    buf_appendf(&local, "%s/%s.%s", dir, id, ext);
    free(dir);
    free(id);
    int rc = file_write_atomic(local.data, data.data, data.len);
    size_t len = data.len;
    buf_free(&data);
    if (rc != 0) {
        e = tool_err("cannot stage %s locally at %s", path, local.data);
        buf_free(&local);
        free(path);
        return e;
    }
    env->pending_images[env->n_pending_images++] = buf_detach(&local);
    buf_t b;
    buf_init(&b);
    buf_appendf(&b,
                "Image loaded: %s (%s, %zu bytes). The pixels follow in the "
                "next user message — describe what you see; do not call "
                "read_file on this path.",
                path, mime, len);
    free(path);
    return buf_detach(&b);
}

static char *r_terminal(tools_env *env, yyjson_val *args) {
    const char *cmd = jget_str(args, "command");
    if (!cmd || !*cmd) return tool_err("missing command");
    int64_t timeout_s = jget_int(args, "timeout_s", 120);
    if (timeout_s <= 0 || timeout_s > 600) timeout_s = 120;
    buf_t s, out, res;
    buf_init(&s);
    buf_init(&out);
    buf_init(&res);
    if (jget_bool(args, "background", false)) {
        /* log lives on the remote host; read_file reaches it there */
        char *id = gen_id();
        buf_appends(&s, "mkdir -p \"$HOME/.tny-bg\" && l=\"$HOME/.tny-bg/");
        buf_appends(&s, id);
        buf_appends(&s, ".log\" && nohup sh -c ");
        ssh_shell_quote(&s, cmd);
        buf_appends(&s, " >\"$l\" 2>&1 </dev/null & echo $! && echo \"$l\"");
        free(id);
        bool tr, to;
        int rc = run(env, s.data, NULL, 0, 30, &out, &tr, &to);
        buf_free(&s);
        chomp(&out);
        if (rc != 0) {
            buf_appendf(&res, "error: could not start background command: %s",
                        out.data ? out.data : "");
        } else {
            char *nl = out.data ? strchr(out.data, '\n') : NULL;
            if (nl) *nl++ = 0;
            buf_appendf(&res,
                        "started in background on %s: pid %s\ncwd: %s\nlog: %s\n"
                        "Check progress with read_file on the log.",
                        env->ctx->ssh_host, out.data ? out.data : "?", env->ctx->ssh_cwd,
                        nl ? nl : "?");
        }
        buf_free(&out);
        return buf_detach(&res);
    }
    bool truncated, timed_out;
    int code = run(env, cmd, NULL, 0, (int)timeout_s, &out, &truncated, &timed_out);
    if (timed_out)
        buf_appendf(&res, "(timed out after %llds and was killed)\n", (long long)timeout_s);
    buf_appendf(&res, "exit code: %d\n", code);
    if (out.len) {
        buf_appends(&res, "output:\n");
        buf_append(&res, out.data, out.len);
        if (truncated) buf_appends(&res, "\n…(output truncated)");
    } else {
        buf_appends(&res, "(no output)");
    }
    buf_free(&out);
    char *bounded = tool_bound_result(env, res.data, res.len);
    buf_free(&res);
    return bounded;
}

char *tool_ssh_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled) {
    *handled = true;
    if (!env->ctx->ssh_host) {
        *handled = false;
        return NULL;
    }
    if (strcmp(name, "list_files") == 0) return r_list_files(env, args);
    if (strcmp(name, "glob_files") == 0) return r_glob_files(env, args);
    if (strcmp(name, "grep_files") == 0) return r_grep_files(env, args);
    if (strcmp(name, "read_file") == 0) return r_read_file(env, args);
    if (strcmp(name, "read_image") == 0) return r_read_image(env, args);
    if (strcmp(name, "write_file") == 0) return r_write_file(env, args);
    if (strcmp(name, "edit_file") == 0) return r_edit_file(env, args);
    if (strcmp(name, "delete_file") == 0) return r_simple_path_op(env, args, "delete");
    if (strcmp(name, "create_folder") == 0) return r_simple_path_op(env, args, "mkdir");
    if (strcmp(name, "file_info") == 0) return r_simple_path_op(env, args, "info");
    if (strcmp(name, "rename_file") == 0) return r_two_path_op(env, args, false);
    if (strcmp(name, "copy_file") == 0) return r_two_path_op(env, args, true);
    if (strcmp(name, "semantic_search") == 0) return r_semantic_search(env, args);
    if (strcmp(name, "terminal") == 0) return r_terminal(env, args);
    if (strcmp(name, "open_file") == 0)
        return tool_err("open_file is not available over --ssh (no desktop on %s)",
                        env->ctx->ssh_host);
    if (strcmp(name, "install_skill") == 0)
        return tool_err("install_skill reads local directories; not available over --ssh");
    *handled = false; /* memory, skills, mcp, subagent, web, … stay local */
    return NULL;
}
