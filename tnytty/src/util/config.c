#include "util/config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_MAX_BYTES (64 * 1024)

/* Dark theme; every entry 1..15 is >= 4.5:1 against the default
 * background (docs/config.md carries the measured table). Index 0 is a
 * background color, so it is deliberately not contrasty. */
static const uint32_t default_palette[TT_PALETTE_LEN] = {
    0x2a2f3a, 0xf07178, 0x9ece6a, 0xe0c980, 0x7aa2f7, 0xc792ea, 0x56cfd8, 0xb9bfca,
    0x7a8296, 0xff8b92, 0xb4f08a, 0xffe08a, 0x9fc1ff, 0xe0b0ff, 0x7fe6ee, 0xeef1f7,
};

void tt_config_defaults(tt_config *c) {
    memset(c, 0, sizeof *c);
    c->font[0] = '\0';
    c->font_size = 13.0;
    c->titlebar = TT_TITLEBAR_TRANSPARENT;
    c->padding = 8;
    c->fg = 0xd7dae3;
    c->bg = 0x14161f;
    c->divider = 0x3a4152;
    memcpy(c->palette, default_palette, sizeof c->palette);
    c->bold_brightens = true;
    c->copy_on_select = true;
    c->status_bar = true;
}

static void fail(char *err, size_t errcap, const char *fmt, const char *a, const char *b) {
    if (err && errcap) snprintf(err, errcap, fmt, a, b);
}

static bool parse_int(const char *v, long *out, long lo, long hi) {
    char *end = NULL;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno || !end || end == v || *end || n < lo || n > hi) return false;
    *out = n;
    return true;
}

static bool parse_dbl(const char *v, double *out, double lo, double hi) {
    char *end = NULL;
    errno = 0;
    double n = strtod(v, &end);
    if (errno || !end || end == v || *end || !(n >= lo) || !(n <= hi)) return false;
    *out = n;
    return true;
}

/* #rrggbb or rrggbb. */
static bool parse_color(const char *v, uint32_t *out) {
    if (*v == '#') v++;
    if (strlen(v) != 6) return false;
    uint32_t n = 0;
    for (int i = 0; i < 6; i++) {
        char ch = v[i];
        int d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        else return false;
        n = (n << 4) | (uint32_t)d;
    }
    *out = n;
    return true;
}

static bool parse_bool(const char *v, bool *out) {
    if (strcmp(v, "true") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "1") == 0) *out = true;
    else if (strcmp(v, "false") == 0 || strcmp(v, "no") == 0 || strcmp(v, "0") == 0) *out = false;
    else return false;
    return true;
}

int tt_config_set(tt_config *c, const char *key, const char *value, char *err, size_t errcap) {
    if (strncmp(key, "palette", 7) == 0) {
        long idx = 0;
        if (parse_int(key + 7, &idx, 0, TT_PALETTE_LEN - 1)) {
            if (!parse_color(value, &c->palette[idx])) {
                fail(err, errcap, "config: %s: %s is not a #rrggbb color", key, value);
                return -1;
            }
            return 0;
        }
    }
    if (strcmp(key, "foreground") == 0 || strcmp(key, "background") == 0 ||
        strcmp(key, "divider") == 0) {
        uint32_t *slot = key[0] == 'f' ? &c->fg : key[0] == 'b' ? &c->bg : &c->divider;
        if (!parse_color(value, slot)) {
            fail(err, errcap, "config: %s: %s is not a #rrggbb color", key, value);
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "bold-brightens") == 0 || strcmp(key, "copy-on-select") == 0 ||
        strcmp(key, "status-bar") == 0) {
        bool *slot = key[0] == 'b'   ? &c->bold_brightens
                     : key[0] == 'c' ? &c->copy_on_select
                                     : &c->status_bar;
        if (!parse_bool(value, slot)) {
            fail(err, errcap, "config: %s: %s is not true or false", key, value);
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "font") == 0) {
        if (strlen(value) >= sizeof c->font) {
            fail(err, errcap, "config: %s: name too long%s", key, "");
            return -1;
        }
        snprintf(c->font, sizeof c->font, "%s", value);
        return 0;
    }
    if (strcmp(key, "font-size") == 0) {
        double n = 0;
        if (!parse_dbl(value, &n, 4.0, 288.0)) {
            fail(err, errcap, "config: font-size: %s is not a size in 4..288%s", value, "");
            return -1;
        }
        c->font_size = n;
        return 0;
    }
    if (strcmp(key, "padding") == 0) {
        long n = 0;
        if (!parse_int(value, &n, 0, 256)) {
            fail(err, errcap, "config: padding: %s is not an integer in 0..256%s", value, "");
            return -1;
        }
        c->padding = (int)n;
        return 0;
    }
    if (strcmp(key, "macos-titlebar") == 0) {
        if (strcmp(value, "transparent") == 0) c->titlebar = TT_TITLEBAR_TRANSPARENT;
        else if (strcmp(value, "opaque") == 0) c->titlebar = TT_TITLEBAR_OPAQUE;
        else {
            fail(err, errcap, "config: macos-titlebar: %s is not %s", value,
                 "'transparent' or 'opaque'");
            return -1;
        }
        return 0;
    }
    fail(err, errcap, "config: unknown key %s%s", key, "");
    return 1; /* unknown: caller decides (warn from a file, error from a flag) */
}

static char *trim(char *s, size_t *len_out) {
    size_t len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) s[--len] = '\0';
    while (*s == ' ' || *s == '\t') {
        s++;
        len--;
    }
    if (len_out) *len_out = len;
    return s;
}

int tt_config_parse(tt_config *c, const char *text, size_t len, char *err, size_t errcap,
                    tt_config_warn_fn warn, void *warn_user) {
    char line[512];
    size_t off = 0;
    int lineno = 0;
    while (off < len) {
        size_t end = off;
        while (end < len && text[end] != '\n') end++;
        size_t n = end - off;
        lineno++;
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, text + off, n);
        line[n] = '\0';
        off = end < len ? end + 1 : len;

        char *p = trim(line, NULL);
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) {
            if (warn) {
                char msg[600];
                snprintf(msg, sizeof msg, "config:%d: no '=' in %s", lineno, p);
                warn(warn_user, msg);
            }
            continue;
        }
        *eq = '\0';
        char *key = trim(p, NULL);
        char *value = trim(eq + 1, NULL);
        char kerr[256];
        kerr[0] = '\0';
        int rc = tt_config_set(c, key, value, kerr, sizeof kerr);
        if (rc > 0) {
            if (warn) {
                char msg[600];
                snprintf(msg, sizeof msg, "config:%d: %s", lineno, kerr);
                warn(warn_user, msg);
            }
            continue;
        }
        if (rc < 0) {
            if (err && errcap) snprintf(err, errcap, "line %d: %s", lineno, kerr);
            return -1;
        }
    }
    return 0;
}

const char *tt_config_path(char *buf, size_t cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(buf, cap, "%s/tnytty/config", xdg);
        return buf;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(buf, cap, "%s/.config/tnytty/config", home);
        return buf;
    }
    return NULL;
}

int tt_config_load(tt_config *c, char *err, size_t errcap, tt_config_warn_fn warn,
                   void *warn_user) {
    tt_config_defaults(c);
    char path[1024];
    if (!tt_config_path(path, sizeof path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) return 0;
        if (err && errcap) snprintf(err, errcap, "%s: %s", path, strerror(errno));
        return -1;
    }
    char *text = malloc(CONFIG_MAX_BYTES);
    if (!text) {
        fclose(f);
        if (err && errcap) snprintf(err, errcap, "%s: out of memory", path);
        return -1;
    }
    size_t len = fread(text, 1, CONFIG_MAX_BYTES, f);
    fclose(f);
    char perr[256];
    perr[0] = '\0';
    int rc = tt_config_parse(c, text, len, perr, sizeof perr, warn, warn_user);
    free(text);
    if (rc != 0 && err && errcap) snprintf(err, errcap, "%s: %s", path, perr);
    return rc;
}
