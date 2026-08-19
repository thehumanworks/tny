#include "core/skills.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *SKILL_ROOTS[] = {
    "skills", ".agents/skills", ".claude/skills", ".codex/skills",
    ".cursor/skills", ".opencode/skills", NULL
};

/* Parse frontmatter: lines between --- markers; name:/description: keys. */
static bool parse_frontmatter(const char *path, char **name, char **desc) {
    size_t len = 0;
    char *data = file_slurp(path, &len);
    if (!data) return false;
    *name = *desc = NULL;
    char *p = data;
    if (strncmp(p, "---", 3) != 0) { free(data); return false; }
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
    if (!*name) { free(*desc); *desc = NULL; return false; }
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
                    if (strcmp((*arr)[i].name, name) == 0) { dup = true; break; }
                if (dup) { free(name); free(desc); }
                else {
                    *arr = realloc(*arr, sizeof(skill_meta) * (size_t)(*n + 1));
                    (*arr)[*n].name = name;
                    (*arr)[*n].description = desc;
                    (*arr)[*n].dir = xstrdup(sd);
                    (*n)++;
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
        free(skills_dir); free(dst); free(name); free(desc);
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
