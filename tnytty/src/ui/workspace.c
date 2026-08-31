#include "ui/workspace.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WS_VERSION 1u

static const unsigned char ws_magic[8] = {'T', 'N', 'Y', 'T', 'T', 'Y', 'W', 'S'};

typedef struct {
    unsigned char magic[8];
    uint32_t version;
    uint32_t active;
    uint32_t tabs;
} ws_header;

typedef struct {
    int32_t root;
    int32_t focus;
    uint32_t nodes;
} ws_tab_header;

typedef struct {
    uint8_t dir;
    uint8_t reserved[3];
    int32_t ratio;
    int32_t a;
    int32_t b;
    char session_id[9];
    unsigned char padding[3];
} ws_node;

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
}

void tt_workspace_init(tt_workspace *ws) {
    memset(ws, 0, sizeof *ws);
    ws->active = -1;
}

static int path_join(char *out, size_t cap, const char *base, const char *suffix) {
    int n = snprintf(out, cap, "%s/%s", base, suffix);
    if (n < 0 || (size_t)n >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int tt_workspace_path(char *out, size_t cap) {
    const char *state = getenv("TNYTTY_STATE_DIR");
    if (state && *state) return path_join(out, cap, state, "workspace");
    state = getenv("XDG_STATE_HOME");
    if (state && *state) return path_join(out, cap, state, "tnytty/workspace");
    const char *home = getenv("HOME");
    if (!home || !*home) {
        errno = ENOENT;
        return -1;
    }
    return path_join(out, cap, home, ".local/state/tnytty/workspace");
}

static int valid_id(const char id[9]) {
    for (int i = 0; i < 8; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return id[8] == '\0';
}

static int walk_tab(const tt_workspace_tab *tab, int at, unsigned char *seen) {
    if (at < 0 || at >= tab->node_count || seen[at]) return -1;
    seen[at] = 1;
    const tt_workspace_node *n = &tab->nodes[at];
    if (n->ratio < 1 || n->ratio > 999) return -1;
    if (n->dir == TT_SPLIT_LEAF)
        return n->a == -1 && n->b == -1 && valid_id(n->session_id) ? 0 : -1;
    if (n->dir != TT_SPLIT_VERT && n->dir != TT_SPLIT_HORZ) return -1;
    if (n->session_id[0] != '\0') return -1;
    return walk_tab(tab, n->a, seen) == 0 && walk_tab(tab, n->b, seen) == 0 ? 0 : -1;
}

int tt_workspace_validate(const tt_workspace *ws, char *err, size_t errcap) {
    if (!ws || ws->tab_count < 0 || ws->tab_count > TT_WORKSPACE_MAX_TABS ||
        (ws->tab_count == 0 ? ws->active != -1 : ws->active < 0 || ws->active >= ws->tab_count)) {
        set_err(err, errcap, "invalid workspace tab selection");
        return -1;
    }
    for (int i = 0; i < ws->tab_count; i++) {
        const tt_workspace_tab *tab = &ws->tabs[i];
        if (tab->node_count < 1 || tab->node_count > TT_WORKSPACE_MAX_NODES || tab->root < 0 ||
            tab->root >= tab->node_count || tab->focus < 0 || tab->focus >= tab->node_count ||
            tab->nodes[tab->focus].dir != TT_SPLIT_LEAF) {
            set_err(err, errcap, "invalid workspace tab tree");
            return -1;
        }
        unsigned char seen[TT_WORKSPACE_MAX_NODES] = {0};
        if (walk_tab(tab, tab->root, seen) != 0) {
            set_err(err, errcap, "invalid workspace split tree");
            return -1;
        }
        for (int n = 0; n < tab->node_count; n++) {
            if (!seen[n]) {
                set_err(err, errcap, "workspace split tree has unreachable nodes");
                return -1;
            }
        }
    }
    return 0;
}

static int mkdir_one(const char *path) {
    if (mkdir(path, 0700) == 0) return 0;
    if (errno != EEXIST) return -1;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int ensure_dir(const char *path) {
    char buf[1024];
    size_t len = strlen(path);
    if (len >= sizeof buf) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, len + 1);
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf) return mkdir_one(path);
    *slash = '\0';
    if (mkdir_one(path) == 0) return 0;
    if (errno != ENOENT || ensure_dir(buf) != 0) return -1;
    return mkdir_one(path);
}

static int ensure_parent(const char *path) {
    char buf[1024];
    size_t len = strlen(path);
    if (len >= sizeof buf) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, len + 1);
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf) return 0;
    *slash = '\0';
    return ensure_dir(buf);
}

static int write_full(int fd, const void *data, size_t len) {
    const unsigned char *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_full(int fd, void *data, size_t len) {
    unsigned char *p = data;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            errno = EINVAL;
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_doc(int fd, const tt_workspace *ws) {
    ws_header h;
    memcpy(h.magic, ws_magic, sizeof h.magic);
    h.version = WS_VERSION;
    h.active = (uint32_t)ws->active;
    h.tabs = (uint32_t)ws->tab_count;
    if (write_full(fd, &h, sizeof h) != 0) return -1;
    for (int i = 0; i < ws->tab_count; i++) {
        const tt_workspace_tab *tab = &ws->tabs[i];
        ws_tab_header th = {tab->root, tab->focus, (uint32_t)tab->node_count};
        if (write_full(fd, &th, sizeof th) != 0) return -1;
        for (int j = 0; j < tab->node_count; j++) {
            const tt_workspace_node *src = &tab->nodes[j];
            ws_node n;
            memset(&n, 0, sizeof n);
            n.dir = (uint8_t)src->dir;
            n.ratio = src->ratio;
            n.a = src->a;
            n.b = src->b;
            memcpy(n.session_id, src->session_id, sizeof n.session_id);
            if (write_full(fd, &n, sizeof n) != 0) return -1;
        }
    }
    return 0;
}

int tt_workspace_save(const char *path, const tt_workspace *ws, char *err, size_t errcap) {
    if (tt_workspace_validate(ws, err, errcap) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (ensure_parent(path) != 0) {
        set_err(err, errcap, "cannot create tnytty state directory");
        return -1;
    }
    char tmp[1100];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof tmp) {
        errno = ENAMETOOLONG;
        set_err(err, errcap, "tnytty state path is too long");
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        set_err(err, errcap, "cannot create tnytty workspace state");
        return -1;
    }
    int rc = write_doc(fd, ws);
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    int saved = errno;
    if (close(fd) != 0 && rc == 0) {
        rc = -1;
        saved = errno;
    }
    if (rc == 0 && rename(tmp, path) != 0) {
        rc = -1;
        saved = errno;
    }
    if (rc != 0) {
        unlink(tmp);
        errno = saved;
        set_err(err, errcap, "cannot save tnytty workspace state");
    }
    return rc;
}

static int trailing_byte(int fd) {
    unsigned char byte;
    ssize_t n;
    do n = read(fd, &byte, 1);
    while (n < 0 && errno == EINTR);
    if (n == 0) return 0;
    errno = EINVAL;
    return -1;
}

int tt_workspace_load(const char *path, tt_workspace *ws, char *err, size_t errcap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) return 1;
        set_err(err, errcap, "cannot read tnytty workspace state");
        return -1;
    }
    tt_workspace out;
    tt_workspace_init(&out);
    ws_header h;
    int rc = read_full(fd, &h, sizeof h);
    if (rc == 0 && (memcmp(h.magic, ws_magic, sizeof h.magic) != 0 || h.version != WS_VERSION ||
                    h.tabs > TT_WORKSPACE_MAX_TABS)) {
        errno = EINVAL;
        rc = -1;
    }
    if (rc == 0) {
        out.tab_count = (int)h.tabs;
        out.active = out.tab_count ? (int)h.active : -1;
    }
    for (int i = 0; rc == 0 && i < out.tab_count; i++) {
        ws_tab_header th;
        if (read_full(fd, &th, sizeof th) != 0 || th.nodes < 1 ||
            th.nodes > TT_WORKSPACE_MAX_NODES) {
            rc = -1;
            break;
        }
        tt_workspace_tab *tab = &out.tabs[i];
        tab->root = th.root;
        tab->focus = th.focus;
        tab->node_count = (int)th.nodes;
        for (int j = 0; rc == 0 && j < tab->node_count; j++) {
            ws_node n;
            if (read_full(fd, &n, sizeof n) != 0) {
                rc = -1;
                break;
            }
            tt_workspace_node *dst = &tab->nodes[j];
            dst->dir = (tt_split_dir)n.dir;
            dst->ratio = n.ratio;
            dst->a = n.a;
            dst->b = n.b;
            memcpy(dst->session_id, n.session_id, sizeof dst->session_id);
        }
    }
    if (rc == 0) rc = trailing_byte(fd);
    int saved = errno;
    close(fd);
    if (rc == 0) rc = tt_workspace_validate(&out, err, errcap);
    if (rc != 0) {
        errno = saved ? saved : EINVAL;
        set_err(err, errcap, "invalid tnytty workspace state");
        return -1;
    }
    *ws = out;
    return 0;
}
