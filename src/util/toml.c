/* toml.c — bounded TOML subset → yyjson for Codex and Grok MCP imports. */
#include "util/toml.h"
#include "util/util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static const char *line_end(const char *p, const char *end) {
    while (p < end && *p != '\n' && *p != '\r') p++;
    return p;
}

static yyjson_mut_val *ensure_obj(yyjson_mut_doc *doc, yyjson_mut_val *parent, const char *key,
                                  size_t klen) {
    yyjson_mut_val *cur = yyjson_mut_obj_getn(parent, key, klen);
    if (cur && yyjson_mut_is_obj(cur)) return cur;
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    if (!obj) return NULL;
    if (cur) yyjson_mut_obj_remove_keyn(parent, key, klen);
    yyjson_mut_val *k = yyjson_mut_strncpy(doc, key, klen);
    if (!k || !yyjson_mut_obj_add(parent, k, obj)) return NULL;
    return obj;
}

/* Walk dotted key `a.b.c` creating objects; *leaf_key / *leaf_len is the last
 * segment. Returns the parent object that should own the leaf. */
static yyjson_mut_val *walk_dotted(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *key,
                                   size_t klen, const char **leaf_key, size_t *leaf_len) {
    yyjson_mut_val *cur = root;
    const char *seg = key;
    const char *end = key + klen;
    for (;;) {
        const char *dot = memchr(seg, '.', (size_t)(end - seg));
        if (!dot) {
            *leaf_key = seg;
            *leaf_len = (size_t)(end - seg);
            return cur;
        }
        cur = ensure_obj(doc, cur, seg, (size_t)(dot - seg));
        if (!cur) return NULL;
        seg = dot + 1;
    }
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *unquote_basic(const char *s, size_t n, size_t *out_len) {
    char *out = malloc(n + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\') {
            out[o++] = s[i];
            continue;
        }
        if (i + 1 >= n) {
            free(out);
            return NULL;
        }
        char e = s[++i];
        switch (e) {
        case 'b': out[o++] = '\b'; break;
        case 't': out[o++] = '\t'; break;
        case 'n': out[o++] = '\n'; break;
        case 'f': out[o++] = '\f'; break;
        case 'r': out[o++] = '\r'; break;
        case '"':
        case '\\': out[o++] = e; break;
        case 'u': {
            if (i + 4 >= n) {
                free(out);
                return NULL;
            }
            unsigned cp = 0;
            for (int k = 0; k < 4; k++) {
                int nib = hex_nibble(s[++i]);
                if (nib < 0) {
                    free(out);
                    return NULL;
                }
                cp = (cp << 4) | (unsigned)nib;
            }
            if (cp < 0x80) out[o++] = (char)cp;
            else if (cp < 0x800) {
                out[o++] = (char)(0xC0 | (cp >> 6));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else {
                out[o++] = (char)(0xE0 | (cp >> 12));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            }
            break;
        }
        default: free(out); return NULL;
        }
    }
    out[o] = 0;
    *out_len = o;
    return out;
}

/* Nesting cap: malformed foreign config must fail parse, never overflow the
 * stack (the importer treats a NULL parse as "source disabled"). */
#define TOML_MAX_DEPTH 32

static yyjson_mut_val *parse_value(yyjson_mut_doc *doc, const char **pp, const char *end,
                                   int depth);

static yyjson_mut_val *parse_string(yyjson_mut_doc *doc, const char **pp, const char *end) {
    const char *p = *pp;
    if (p >= end) return NULL;
    char q = *p;
    if (q != '"' && q != '\'') return NULL;
    p++;
    /* triple quotes: treat the interior as a raw string up to matching close */
    if (p + 1 < end && *p == q && p[1] == q) {
        p += 2;
        const char *s = p;
        while (p + 2 < end && !(*p == q && p[1] == q && p[2] == q)) p++;
        if (p + 2 >= end) return NULL;
        yyjson_mut_val *v = yyjson_mut_strncpy(doc, s, (size_t)(p - s));
        *pp = p + 3;
        return v;
    }
    const char *s = p;
    if (q == '\'') {
        while (p < end && *p != '\'') p++;
        if (p >= end) return NULL;
        yyjson_mut_val *v = yyjson_mut_strncpy(doc, s, (size_t)(p - s));
        *pp = p + 1;
        return v;
    }
    while (p < end && *p != '"') {
        if (*p == '\\') {
            p++;
            if (p < end) p++;
            continue;
        }
        p++;
    }
    if (p >= end) return NULL;
    size_t raw = (size_t)(p - s);
    size_t n = 0;
    char *u = unquote_basic(s, raw, &n);
    if (!u) return NULL;
    yyjson_mut_val *v = yyjson_mut_strncpy(doc, u, n);
    free(u);
    *pp = p + 1;
    return v;
}

static yyjson_mut_val *parse_array(yyjson_mut_doc *doc, const char **pp, const char *end,
                                   int depth) {
    const char *p = *pp;
    if (depth > TOML_MAX_DEPTH || p >= end || *p != '[') return NULL;
    p++;
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    if (!arr) return NULL;
    for (;;) {
        p = skip_ws(p, end);
        if (p < end && *p == '#') p = line_end(p, end);
        while (p < end && (*p == '\n' || *p == '\r')) {
            p++;
            p = skip_ws(p, end);
            if (p < end && *p == '#') p = line_end(p, end);
        }
        if (p < end && *p == ']') {
            *pp = p + 1;
            return arr;
        }
        yyjson_mut_val *v = parse_value(doc, &p, end, depth + 1);
        if (!v) return NULL;
        if (!yyjson_mut_arr_append(arr, v)) return NULL;
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p++;
            continue;
        }
        p = skip_ws(p, end);
        if (p < end && *p == ']') {
            *pp = p + 1;
            return arr;
        }
        return NULL;
    }
}

static yyjson_mut_val *parse_inline_table(yyjson_mut_doc *doc, const char **pp, const char *end,
                                          int depth) {
    const char *p = *pp;
    if (depth > TOML_MAX_DEPTH || p >= end || *p != '{') return NULL;
    p++;
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    if (!obj) return NULL;
    for (;;) {
        p = skip_ws(p, end);
        if (p < end && *p == '}') {
            *pp = p + 1;
            return obj;
        }
        const char *ks = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.')) p++;
        size_t klen = (size_t)(p - ks);
        if (!klen) return NULL;
        p = skip_ws(p, end);
        if (p >= end || *p != '=') return NULL;
        p = skip_ws(p + 1, end);
        yyjson_mut_val *v = parse_value(doc, &p, end, depth + 1);
        if (!v) return NULL;
        const char *leaf = NULL;
        size_t llen = 0;
        yyjson_mut_val *parent = walk_dotted(doc, obj, ks, klen, &leaf, &llen);
        if (!parent || !llen) return NULL;
        yyjson_mut_val *k = yyjson_mut_strncpy(doc, leaf, llen);
        if (!k || !yyjson_mut_obj_add(parent, k, v)) return NULL;
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p++;
            continue;
        }
        if (p < end && *p == '}') {
            *pp = p + 1;
            return obj;
        }
        return NULL;
    }
}

static yyjson_mut_val *parse_value(yyjson_mut_doc *doc, const char **pp, const char *end,
                                   int depth) {
    const char *p = skip_ws(*pp, end);
    if (p >= end) return NULL;
    if (*p == '"' || *p == '\'') {
        yyjson_mut_val *v = parse_string(doc, &p, end);
        *pp = p;
        return v;
    }
    if (*p == '[') {
        yyjson_mut_val *v = parse_array(doc, &p, end, depth);
        *pp = p;
        return v;
    }
    if (*p == '{') {
        yyjson_mut_val *v = parse_inline_table(doc, &p, end, depth);
        *pp = p;
        return v;
    }
    if (p + 4 <= end && !memcmp(p, "true", 4) && (p + 4 == end || !isalnum((unsigned char)p[4]))) {
        *pp = p + 4;
        return yyjson_mut_bool(doc, true);
    }
    if (p + 5 <= end && !memcmp(p, "false", 5) && (p + 5 == end || !isalnum((unsigned char)p[5]))) {
        *pp = p + 5;
        return yyjson_mut_bool(doc, false);
    }
    const char *s = p;
    if (*p == '+' || *p == '-') p++;
    bool seen_digit = false, seen_dot = false, seen_exp = false;
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (isdigit(c)) {
            seen_digit = true;
            p++;
            continue;
        }
        if (c == '.' && !seen_dot && !seen_exp) {
            seen_dot = true;
            p++;
            continue;
        }
        if ((c == 'e' || c == 'E') && !seen_exp && seen_digit) {
            seen_exp = true;
            p++;
            if (p < end && (*p == '+' || *p == '-')) p++;
            continue;
        }
        if (c == '_') {
            p++;
            continue;
        } /* TOML numeric underscores */
        break;
    }
    if (!seen_digit) return NULL;
    size_t n = (size_t)(p - s);
    char *tmp = xstrndup(s, n);
    if (!tmp) return NULL;
    /* strip underscores */
    char *w = tmp;
    for (char *r = tmp; *r; r++)
        if (*r != '_') *w++ = *r;
    *w = 0;
    yyjson_mut_val *v;
    if (seen_dot || seen_exp) v = yyjson_mut_real(doc, strtod(tmp, NULL));
    else v = yyjson_mut_sint(doc, strtoll(tmp, NULL, 10));
    free(tmp);
    *pp = p;
    return v;
}

static bool valid_key_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.';
}

static bool mcp_key_recognized(const char *key, size_t len, bool env_scope) {
    if (env_scope) return true;
    static const char *const keys[] = {"command", "args", "env",     "cwd",
                                       "url",     "type", "enabled", NULL};
    for (int i = 0; keys[i]; i++)
        if (strlen(keys[i]) == len && memcmp(keys[i], key, len) == 0) return true;
    return false;
}

static bool mcp_value_type_ok(const char *key, size_t len, bool env_scope, yyjson_mut_val *value) {
    if (env_scope) return yyjson_mut_is_str(value);
    if ((len == 7 && memcmp(key, "command", 7) == 0) || (len == 3 && memcmp(key, "cwd", 3) == 0) ||
        (len == 3 && memcmp(key, "url", 3) == 0) || (len == 4 && memcmp(key, "type", 4) == 0))
        return yyjson_mut_is_str(value);
    if (len == 4 && memcmp(key, "args", 4) == 0) return yyjson_mut_is_arr(value);
    if (len == 3 && memcmp(key, "env", 3) == 0) return yyjson_mut_is_obj(value);
    if (len == 7 && memcmp(key, "enabled", 7) == 0) return yyjson_mut_is_bool(value);
    return true;
}

static bool value_tail_ok(const char *p, const char *end) {
    const char *line = line_end(p, end);
    p = skip_ws(p, line);
    return p == line || *p == '#';
}

yyjson_doc *toml_parse_subset(const char *src, size_t len) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(jallocator());
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    if (!root) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *cur = root;
    bool mcp_scope = false;
    bool mcp_env_scope = false;
    const char *p = src;
    const char *end = src + len;
    while (p < end) {
        if (*p == '\n' || *p == '\r') {
            p++;
            continue;
        }
        p = skip_ws(p, end);
        if (p >= end) break;
        if (*p == '#') {
            p = line_end(p, end);
            continue;
        }
        if (*p == '[') {
            bool arrtab = false;
            p++;
            if (p < end && *p == '[') {
                arrtab = true;
                p++;
            }
            p = skip_ws(p, end);
            const char *ks = p;
            while (p < end && valid_key_char(*p)) p++;
            size_t klen = (size_t)(p - ks);
            mcp_scope = klen > strlen("mcp_servers.") &&
                        memcmp(ks, "mcp_servers.", strlen("mcp_servers.")) == 0;
            /* Only `mcp_servers.<name>.env` is an env table; a server named
             * plain `env` ([mcp_servers.env]) is a server table. */
            mcp_env_scope = mcp_scope && klen > strlen("mcp_servers.") + 4 &&
                            memcmp(ks + klen - 4, ".env", 4) == 0;
            p = skip_ws(p, end);
            if (arrtab) {
                if (p < end && *p == ']') p++;
                p = skip_ws(p, end);
                if (p < end && *p == ']') p++;
                p = line_end(p, end);
                cur = root; /* arrays of tables are ignored */
                mcp_scope = false;
                mcp_env_scope = false;
                (void)klen;
                continue;
            }
            if (p < end && *p == ']') p++;
            else if (mcp_scope) goto malformed;
            else {
                /* Unsupported header syntax (e.g. quoted names): discard the
                 * table body rather than spilling its keys onto the root. */
                p = line_end(p, end);
                mcp_env_scope = false;
                cur = yyjson_mut_obj(doc);
                if (!cur) goto malformed;
                continue;
            }
            p = line_end(p, end);
            if (!klen) {
                cur = root;
                continue;
            }
            const char *leaf = NULL;
            size_t llen = 0;
            yyjson_mut_val *parent = walk_dotted(doc, root, ks, klen, &leaf, &llen);
            if (!parent || !llen) {
                cur = root;
                continue;
            }
            cur = ensure_obj(doc, parent, leaf, llen);
            if (!cur) cur = root;
            continue;
        }
        const char *ks = p;
        if (*p == '"' || *p == '\'') {
            /* quoted key — reuse string parser, then expect = */
            yyjson_mut_val *qk = parse_string(doc, &p, end);
            if (!qk || !yyjson_mut_is_str(qk)) {
                if (mcp_env_scope) goto malformed;
                p = line_end(p, end);
                continue;
            }
            ks = yyjson_mut_get_str(qk);
            size_t klen = yyjson_mut_get_len(qk);
            p = skip_ws(p, end);
            if (p >= end || *p != '=') {
                if (mcp_env_scope) goto malformed;
                p = line_end(p, end);
                continue;
            }
            p = skip_ws(p + 1, end);
            yyjson_mut_val *v = parse_value(doc, &p, end, 0);
            if (!v) {
                if (mcp_env_scope) goto malformed;
                p = line_end(p, end);
                continue;
            }
            if (mcp_env_scope && (!value_tail_ok(p, end) || !mcp_value_type_ok(ks, klen, true, v)))
                goto malformed;
            yyjson_mut_val *k = yyjson_mut_strncpy(doc, ks, klen);
            if (k) yyjson_mut_obj_put(cur, k, v);
            p = line_end(p, end);
            continue;
        }
        while (p < end && valid_key_char(*p)) p++;
        size_t klen = (size_t)(p - ks);
        bool strict = mcp_scope && mcp_key_recognized(ks, klen, mcp_env_scope);
        p = skip_ws(p, end);
        if (!klen || p >= end || *p != '=') {
            if (strict) goto malformed;
            p = line_end(p, end);
            continue;
        }
        p = skip_ws(p + 1, end);
        yyjson_mut_val *v = parse_value(doc, &p, end, 0);
        if (!v) {
            if (strict) goto malformed;
            p = line_end(p, end);
            continue;
        }
        if (strict && (!value_tail_ok(p, end) || !mcp_value_type_ok(ks, klen, mcp_env_scope, v)))
            goto malformed;
        const char *leaf = NULL;
        size_t llen = 0;
        yyjson_mut_val *parent = walk_dotted(doc, cur, ks, klen, &leaf, &llen);
        if (parent && llen) {
            yyjson_mut_val *k = yyjson_mut_strncpy(doc, leaf, llen);
            if (k) yyjson_mut_obj_put(parent, k, v);
        }
        p = line_end(p, end);
    }
    yyjson_doc *im = yyjson_mut_doc_imut_copy(doc, jallocator());
    yyjson_mut_doc_free(doc);
    return im;

malformed:
    yyjson_mut_doc_free(doc);
    return NULL;
}
