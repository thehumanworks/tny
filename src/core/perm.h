/* perm.h — native-loop permission engine (docs/features/permissions.md).
 * Host backends never call this; they only render host requests. */
#ifndef TNY_PERM_H
#define TNY_PERM_H

#include "core/config.h"

typedef enum {
    PERM_ALLOW,      /* run it */
    PERM_DENY,       /* refuse, tell the model */
    PERM_PROMPT      /* unresolved: TUI prompts, ask-mode CLI fails (exit 2) */
} perm_verdict;

typedef struct {
    tny_ctx *ctx;
    /* session grants: tool-name or "bash:<argv0> *" style keys */
    char **grants;
    int    n_grants;
} perm_engine;

perm_engine *perm_new(tny_ctx *ctx);
void perm_free(perm_engine *p);

/* Decide for one tool call. `tool` is the built-in tool name (or
 * "mcp:<server>/<tool>"), `detail` is the command line for terminal or the
 * absolute target path for file tools (may be NULL). */
perm_verdict perm_check(perm_engine *p, const char *tool, const char *detail);

/* Record a session grant ("Yes, and don't ask again"). Dies with process. */
void perm_grant(perm_engine *p, const char *tool, const char *detail);

int perm_grant_count(perm_engine *p);

/* True if this tool is never-sensitive inside the workspace. */
bool perm_tool_is_safe(const char *tool);

/* Is the path inside the workspace or an extra dir? */
bool perm_path_allowed(tny_ctx *ctx, const char *abs_path);

#endif
