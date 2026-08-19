#include "core/perm.h"
#include "util/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *SAFE_TOOLS[] = {
    "list_files", "glob_files", "grep_files", "read_file", "file_info",
    "semantic_search", "read_tool_result", "mcp_search_tools", "mcp_select_tool",
    "mcp_features", "skill", "ask_user_question", NULL
};

bool perm_tool_is_safe(const char *tool) {
    for (int i = 0; SAFE_TOOLS[i]; i++)
        if (strcmp(tool, SAFE_TOOLS[i]) == 0) return true;
    return false;
}

bool perm_path_allowed(tny_ctx *ctx, const char *abs_path) {
    if (path_is_within(ctx->cwd, abs_path)) return true;
    for (int i = 0; i < ctx->n_extra_dirs; i++)
        if (path_is_within(ctx->extra_dirs[i], abs_path)) return true;
    return false;
}

perm_engine *perm_new(tny_ctx *ctx) {
    perm_engine *p = calloc(1, sizeof *p);
    if (p) p->ctx = ctx;
    return p;
}

void perm_free(perm_engine *p) {
    if (!p) return;
    for (int i = 0; i < p->n_grants; i++) free(p->grants[i]);
    free(p->grants);
    free(p);
}

int perm_grant_count(perm_engine *p) { return p->n_grants; }

static char *grant_key(const char *tool, const char *detail) {
    buf_t b;
    buf_init(&b);
    if (strcmp(tool, "terminal") == 0 && detail) {
        /* grant covers the same leading program */
        char prog[128] = {0};
        sscanf(detail, "%127s", prog);
        buf_appendf(&b, "terminal:%s", prog);
    } else {
        buf_appends(&b, tool);
    }
    return buf_detach(&b);
}

void perm_grant(perm_engine *p, const char *tool, const char *detail) {
    char *key = grant_key(tool, detail);
    for (int i = 0; i < p->n_grants; i++)
        if (strcmp(p->grants[i], key) == 0) { free(key); return; }
    p->grants = realloc(p->grants, sizeof(char *) * (size_t)(p->n_grants + 1));
    p->grants[p->n_grants++] = key;
}

static bool grant_hit(perm_engine *p, const char *tool, const char *detail) {
    char *key = grant_key(tool, detail);
    bool hit = false;
    for (int i = 0; i < p->n_grants; i++)
        if (strcmp(p->grants[i], key) == 0) { hit = true; break; }
    free(key);
    return hit;
}

/* Map a tool name onto a rule category used in settings.json:
 * "bash" for terminal, "edit" for write-ish file tools, else tool name. */
static const char *rule_category(const char *tool) {
    if (strcmp(tool, "terminal") == 0 || strcmp(tool, "run_command") == 0) return "bash";
    if (strcmp(tool, "write_file") == 0 || strcmp(tool, "edit_file") == 0 ||
        strcmp(tool, "delete_file") == 0 || strcmp(tool, "rename_file") == 0 ||
        strcmp(tool, "copy_file") == 0 || strcmp(tool, "create_folder") == 0)
        return "edit";
    return tool;
}

/* Look up rules in one permission object. Returns -1 none, 0 deny, 1 allow,
 * 2 ask. Last match wins within the category map. */
static int rules_lookup(yyjson_val *perm, const char *tool, const char *detail) {
    if (!perm) return -1;
    int verdict = -1;
    const char *star = jget_str(perm, "*");
    if (star) {
        if (strcmp(star, "allow") == 0) verdict = 1;
        else if (strcmp(star, "deny") == 0) verdict = 0;
        else verdict = 2;
    }
    yyjson_val *cat = jget(perm, rule_category(tool));
    if (cat && yyjson_is_obj(cat) && detail) {
        size_t idx, max;
        yyjson_val *k, *v;
        yyjson_obj_foreach(cat, idx, max, k, v) {
            const char *pat = yyjson_get_str(k);
            const char *act = yyjson_get_str(v);
            if (!pat || !act) continue;
            if (glob_match(pat, detail)) {
                if (strcmp(act, "allow") == 0) verdict = 1;
                else if (strcmp(act, "deny") == 0) verdict = 0;
                else verdict = 2;
            }
        }
    } else if (cat && yyjson_is_str(cat)) {
        const char *act = yyjson_get_str(cat);
        if (strcmp(act, "allow") == 0) verdict = 1;
        else if (strcmp(act, "deny") == 0) verdict = 0;
        else verdict = 2;
    }
    return verdict;
}

perm_verdict perm_check(perm_engine *p, const char *tool, const char *detail) {
    tny_ctx *ctx = p->ctx;
    if (ctx->perm_mode == TNY_MODE_YOLO) return PERM_ALLOW;

    /* read-only tools are free inside the workspace; path escapes prompt */
    if (perm_tool_is_safe(tool)) {
        if (detail && detail[0] == '/' && !perm_path_allowed(ctx, detail))
            goto sensitive;
        return PERM_ALLOW;
    }

sensitive:;
    /* workspace rules beat user-global; last match wins inside each */
    yyjson_val *sroot = ctx->settings ? yyjson_doc_get_root(ctx->settings) : NULL;
    yyjson_val *global_rules = jget(sroot, "permission");
    yyjson_val *ws = jget(jget(sroot, "workspaces"), ctx->cwd);
    yyjson_val *ws_rules = jget(ws, "permission");

    int v = rules_lookup(ws_rules, tool, detail);
    if (v < 0) v = rules_lookup(global_rules, tool, detail);
    if (v == 1) return PERM_ALLOW;
    if (v == 0) return PERM_DENY;

    if (grant_hit(p, tool, detail)) return PERM_ALLOW;

    if (ctx->perm_mode == TNY_MODE_AUTO) {
        /* Cheap local heuristic: allow writes inside the workspace and
         * clearly-safe read-style commands; everything else stays PROMPT
         * (TUI asks, `ask` fails closed). */
        const char *cat = rule_category(tool);
        if (strcmp(cat, "edit") == 0 && detail && perm_path_allowed(ctx, detail))
            return PERM_ALLOW;
        if (strcmp(cat, "bash") == 0 && detail) {
            static const char *safe_prog[] = {
                "ls", "cat", "head", "tail", "grep", "rg", "find", "wc",
                "git status", "git log", "git diff", "git show", NULL
            };
            for (int i = 0; safe_prog[i]; i++)
                if (str_starts(detail, safe_prog[i])) return PERM_ALLOW;
        }
        if (strcmp(tool, "web_fetch") == 0 || strcmp(tool, "web_search") == 0 ||
            strcmp(tool, "vision") == 0 || strcmp(tool, "open_file") == 0 ||
            strcmp(tool, "memory") == 0)
            return PERM_ALLOW;
    }
    return PERM_PROMPT;
}
