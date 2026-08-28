/* codex_host.c — shared app-server registry (~/.tny/codex-host.json).
 * A tny that spawns `codex app-server` publishes {"ws","pid"} here so later
 * one-shot runs attach instead of paying the ~1 s spawn+initialize again.
 * The file is written by whoever owned the last spawn and treated as
 * untrusted on read: loopback-only URLs, live pid required, and any doubt
 * means "spawn our own". */
#include "backends/codex/codex.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

char *cx_registry_path(void) {
    char *dir = path_tny_dir();
    char *p = path_join(dir, "codex-host.json");
    free(dir);
    return p;
}

/* Strictly `ws://` on 127.0.0.1 or localhost with a bare 1-65535 port (an
 * optional trailing "/" at most). The registry can be scribbled on by
 * anything with the user's uid; never let it point us off-loopback. */
bool cx_ws_url_is_loopback(const char *url) {
    if (!url || !str_starts(url, "ws://")) return false;
    const char *host = url + 5;
    const char *colon = strchr(host, ':');
    if (!colon) return false;
    size_t hlen = (size_t)(colon - host);
    if (hlen != 9 || (strncmp(host, "127.0.0.1", 9) != 0 && strncmp(host, "localhost", 9) != 0))
        return false;
    const char *p = colon + 1;
    long port = 0;
    while (*p >= '0' && *p <= '9') {
        port = port * 10 + (*p - '0');
        if (port > 65535) return false;
        p++;
    }
    if (port <= 0) return false;
    return *p == 0 || (p[0] == '/' && p[1] == 0);
}

int cx_registry_write(const char *ws_url, pid_t pid) {
    char *dir = path_tny_dir();
    if (mkdir_p(dir) != 0) {
        free(dir);
        return -1;
    }
    free(dir);
    char *path = cx_registry_path();
    buf_t j;
    buf_init(&j);
    buf_appends(&j, "{\"ws\":");
    jescape(&j, ws_url);
    buf_appendf(&j, ",\"pid\":%ld}", (long)pid);
    int rc = file_write_atomic(path, j.data, j.len); /* 0600, tmp+rename */
    buf_free(&j);
    free(path);
    return rc;
}

int cx_registry_remove(pid_t pid) {
    char *path = cx_registry_path();
    size_t len = 0;
    char *data = file_slurp(path, &len);
    int rc = -1;
    if (data) {
        yyjson_doc *doc = jparse(data, len);
        free(data);
        if (doc) {
            /* unlink only while the file still names our host; a newer
             * writer's entry must survive us */
            if (jget_int(yyjson_doc_get_root(doc), "pid", 0) == (int64_t)pid) rc = unlink(path);
            yyjson_doc_free(doc);
        }
    }
    free(path);
    return rc;
}

int cx_registry_load(char **ws_out, pid_t *pid_out) {
    *ws_out = NULL;
    char *path = cx_registry_path();
    size_t len = 0;
    char *data = file_slurp(path, &len);
    free(path);
    if (!data) return -1;
    yyjson_doc *doc = jparse(data, len);
    free(data);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *ws = jget_str(root, "ws");
    int64_t pid = jget_int(root, "pid", 0);
    int rc = -1;
    /* EPERM still means "alive"; anything else is a stale entry */
    if (ws && cx_ws_url_is_loopback(ws) && pid > 0 &&
        (kill((pid_t)pid, 0) == 0 || errno == EPERM)) {
        *ws_out = xstrdup(ws);
        if (pid_out) *pid_out = (pid_t)pid;
        rc = 0;
    }
    yyjson_doc_free(doc);
    return rc;
}
