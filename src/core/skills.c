#include "core/skills.h"
#include "core/tools.h"
#include "util/util.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *SKILL_ROOTS[] = {"skills",
                                    ".agents/skills",
                                    ".claude/skills",
                                    ".codex/skills",
                                    ".cursor/skills",
                                    ".opencode/skills",
                                    NULL};

/* Parse frontmatter: lines between --- markers; name:/description: keys. */
static bool parse_frontmatter(const char *path, char **name, char **desc) {
    size_t len = 0;
    char *data = file_slurp(path, &len);
    if (!data) return false;
    *name = *desc = NULL;
    char *p = data;
    if (strncmp(p, "---", 3) != 0) {
        free(data);
        return false;
    }
    p = strchr(p, '\n');
    while (p) {
        p++;
        if (strncmp(p, "---", 3) == 0) break;
        char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (strncmp(p, "name:", 5) == 0) {
            char *v = xstrndup(p + 5, ll - 5);
            *name = xstrdup(str_trim(v));
            free(v);
        } else if (strncmp(p, "description:", 12) == 0) {
            char *v = xstrndup(p + 12, ll - 12);
            *desc = xstrdup(str_trim(v));
            free(v);
        }
        p = nl;
    }
    free(data);
    if (!*name) {
        free(*desc);
        *desc = NULL;
        return false;
    }
    if (!*desc) *desc = xstrdup("");
    return true;
}

static void scan_root(const char *root, skill_meta **arr, int *n) {
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char *sd = path_join(root, e->d_name);
        char *sf = path_join(sd, "SKILL.md");
        if (file_exists(sf)) {
            char *name = NULL, *desc = NULL;
            if (parse_frontmatter(sf, &name, &desc)) {
                /* dedupe by name: nearer root wins (first found) */
                bool dup = false;
                for (int i = 0; i < *n; i++)
                    if (strcmp((*arr)[i].name, name) == 0) {
                        dup = true;
                        break;
                    }
                if (dup) {
                    free(name);
                    free(desc);
                } else {
                    skill_meta *grown = realloc(*arr, sizeof(skill_meta) * (size_t)(*n + 1));
                    if (!grown) {
                        free(name);
                        free(desc);
                    } else {
                        *arr = grown;
                        (*arr)[*n].name = name;
                        (*arr)[*n].description = desc;
                        (*arr)[*n].dir = xstrdup(sd);
                        (*n)++;
                    }
                }
            }
        }
        free(sf);
        free(sd);
    }
    closedir(d);
}

skill_meta *skills_discover(tny_ctx *ctx, int *count) {
    skill_meta *arr = NULL;
    int n = 0;
    char *home = path_home();
    size_t home_len = strlen(home);

    /* workspace upward, stop before $HOME (narrowest first so it wins) */
    char *cur = xstrdup(ctx->cwd);
    for (;;) {
        bool at_home = strcmp(cur, home) == 0;
        bool above_home = strncmp(home, cur, strlen(cur)) == 0 && strlen(cur) < home_len;
        if (at_home || above_home) break;
        for (int i = 0; SKILL_ROOTS[i]; i++) {
            char *root = path_join(cur, SKILL_ROOTS[i]);
            scan_root(root, &arr, &n);
            free(root);
        }
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) break;
        *slash = 0;
    }
    free(cur);

    /* user level: ~/.tny/skills and hidden names under $HOME */
    char *managed = path_join(ctx->tny_dir, "skills");
    scan_root(managed, &arr, &n);
    free(managed);
    for (int i = 0; SKILL_ROOTS[i]; i++) {
        if (SKILL_ROOTS[i][0] != '.') continue;
        char *root = path_join(home, SKILL_ROOTS[i]);
        scan_root(root, &arr, &n);
        free(root);
    }
    free(home);
    *count = n;
    return arr;
}

void skills_free(skill_meta *s, int count) {
    for (int i = 0; i < count; i++) {
        free(s[i].name);
        free(s[i].description);
        free(s[i].dir);
    }
    free(s);
}

char *skills_load(tny_ctx *ctx, const char *name) {
    int n = 0;
    skill_meta *all = skills_discover(ctx, &n);
    char *body = NULL;
    for (int i = 0; i < n; i++) {
        if (strcmp(all[i].name, name) == 0) {
            char *sf = path_join(all[i].dir, "SKILL.md");
            body = file_slurp(sf, NULL);
            free(sf);
            break;
        }
    }
    skills_free(all, n);
    return body;
}

int skills_install(tny_ctx *ctx, const char *src_dir, char *err, size_t errlen) {
    char *sf = path_join(src_dir, "SKILL.md");
    char *name = NULL, *desc = NULL;
    if (!parse_frontmatter(sf, &name, &desc)) {
        snprintf(err, errlen, "%s has no valid SKILL.md frontmatter", src_dir);
        free(sf);
        return -1;
    }
    free(sf);
    char *skills_dir = path_join(ctx->tny_dir, "skills");
    char *dst = path_join(skills_dir, name);
    mkdir_p(dst);
    /* copy files (flat + one level is enough for v1) */
    DIR *d = opendir(src_dir);
    if (!d) {
        snprintf(err, errlen, "cannot open %s", src_dir);
        free(skills_dir);
        free(dst);
        free(name);
        free(desc);
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char *src = path_join(src_dir, e->d_name);
        struct stat st;
        if (stat(src, &st) == 0 && S_ISREG(st.st_mode)) {
            size_t len = 0;
            char *data = file_slurp(src, &len);
            if (data) {
                char *df = path_join(dst, e->d_name);
                file_write_atomic(df, data, len);
                free(df);
                free(data);
            }
        }
        free(src);
    }
    closedir(d);
    free(skills_dir);
    free(dst);
    free(name);
    free(desc);
    return 0;
}

/* ---- mention injection (docs/adr/0056) ---- */

static bool name_char(unsigned char c) { return isalnum(c) || c == '-' || c == '_'; }

size_t skills_mention_pos(const char *text, const char *name) {
    if (!text || !name || !*name) return SIZE_MAX;
    size_t nlen = strlen(name);
    for (const char *p = text; *p; p++) {
        if (*p != '/' && *p != '$') continue;
        if (p != text && !isspace((unsigned char)p[-1])) continue;
        if (strncmp(p + 1, name, nlen) != 0) continue;
        unsigned char after = (unsigned char)p[1 + nlen];
        if (after == 0 || isspace(after)) return (size_t)(p - text);
        if (name_char(after) || after == '/') continue;
        if (ispunct(after)) return (size_t)(p - text);
    }
    return SIZE_MAX;
}

int skills_mentioned(const char *text, const skill_meta *skills, int n, int *out, int max) {
    int found = 0;
    size_t *pos = calloc(n > 0 ? (size_t)n : 1, sizeof *pos);
    if (!pos) return 0;
    for (int i = 0; i < n; i++) pos[i] = skills_mention_pos(text, skills[i].name);
    /* order of first appearance: repeatedly take the smallest offset */
    for (;;) {
        int best = -1;
        for (int i = 0; i < n; i++)
            if (pos[i] != SIZE_MAX && (best < 0 || pos[i] < pos[best])) best = i;
        if (best < 0 || found >= max) break;
        out[found++] = best;
        pos[best] = SIZE_MAX;
    }
    free(pos);
    return found;
}

static void names_push(char ***names, int *n, const char *name) {
    char **grown = realloc(*names, sizeof(char *) * (size_t)(*n + 1));
    if (!grown) return;
    *names = grown;
    (*names)[(*n)++] = xstrdup(name);
}

void skills_names_free(char **names, int n) {
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

char *skills_inject(tny_ctx *ctx, tny_session_state *session, const char *prompt, bool native,
                    char ***names, int *n_names) {
    *names = NULL;
    *n_names = 0;
    if (!prompt || ctx->library_mode) return NULL;
    /* cheap pre-check: no sigil, no scan (skills_discover walks the tree) */
    if (!strchr(prompt, '/') && !strchr(prompt, '$')) return NULL;
    int n = 0;
    skill_meta *all = skills_discover(ctx, &n);
    /* order of first appearance: take the smallest remaining offset */
    size_t *pos = calloc(n > 0 ? (size_t)n : 1, sizeof *pos);
    if (!pos) {
        skills_free(all, n);
        return NULL;
    }
    for (int i = 0; i < n; i++) pos[i] = skills_mention_pos(prompt, all[i].name);
    buf_t b;
    buf_init(&b);
    for (;;) {
        int best = -1;
        for (int i = 0; i < n; i++)
            if (pos[i] != SIZE_MAX && (best < 0 || pos[i] < pos[best])) best = i;
        if (best < 0) break;
        pos[best] = SIZE_MAX;
        const skill_meta *sk = &all[best];
        char *sf = path_join(sk->dir, "SKILL.md");
        if (session && session_skill_injected(session, sk->name)) {
            buf_appendf(&b,
                        "<skill name=\"%s\" path=\"%s\" status=\"already loaded earlier in this "
                        "conversation; its instructions are still in context\"/>\n",
                        sk->name, sf);
            names_push(names, n_names, sk->name);
            free(sf);
            continue;
        }
        size_t len = 0;
        char *body = file_slurp(sf, &len);
        if (!body) {
            free(sf);
            continue;
        }
        buf_appendf(&b, "<skill name=\"%s\" path=\"%s\">\n", sk->name, sf);
        size_t maxb = ctx->max_tool_result_bytes;
        if (len <= maxb) {
            buf_append(&b, body, len);
        } else if (native) {
            tools_env env = {0};
            env.ctx = ctx;
            env.session = session;
            char *bounded = tool_bound_result(&env, body, len);
            buf_appends(&b, bounded);
            free(bounded);
            buf_appendf(&b, "\nThe `skill` tool or read_file on %s shows the rest.", sf);
        } else {
            buf_append(&b, body, maxb / 2);
            buf_appendf(&b, "\n…[truncated: %zu of %zu bytes shown; read the rest from %s]",
                        maxb / 2, len, sf);
        }
        if (b.len && b.data[b.len - 1] != '\n') buf_appends(&b, "\n");
        buf_appends(&b, "</skill>\n");
        names_push(names, n_names, sk->name);
        free(body);
        free(sf);
    }
    free(pos);
    skills_free(all, n);
    if (!*n_names || buf_oom(&b)) {
        buf_free(&b);
        skills_names_free(*names, *n_names);
        *names = NULL;
        *n_names = 0;
        return NULL;
    }
    buf_appends(&b, "\n");
    buf_appends(&b, prompt);
    return buf_detach(&b);
}
