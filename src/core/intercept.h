/* intercept.h — in-process dispatch of first-party `tny` verbs typed into the
 * terminal tool (docs/adr/0063).
 *
 * When tny is the harness, a nested `tny` process has no permission engine,
 * no session grants, no undo record, no warmed MCP client, and no `--ssh`
 * context. Recognising a single simple `tny …` command inside `terminal` and
 * running it through the same code the typed tools use restores all five.
 * Recognition is deliberately narrow: anything the parser is not certain
 * about is handed to /bin/sh unchanged. */
#ifndef TNY_INTERCEPT_H
#define TNY_INTERCEPT_H

#include "core/tools.h"

typedef enum {
    TNY_INTERCEPT_EDIT = 1,
    TNY_INTERCEPT_MCP_CALL,
    TNY_INTERCEPT_MCP_DESCRIBE, /* `tny mcp tools SERVER` / `describe SERVER/TOOL` */
    TNY_INTERCEPT_MEMORY,
    TNY_INTERCEPT_SKILL,
    TNY_INTERCEPT_IMAGE_ATTACH,
    TNY_INTERCEPT_ASK_USER,
    TNY_INTERCEPT_SPEAK,
    TNY_INTERCEPT_REFUSED, /* recognised and rejected; `message` says why */
} tny_intercept_kind;

typedef struct tny_intercept {
    tny_intercept_kind kind;
    char *label;           /* "tny edit docs/x.md": event detail and prompt */
    char *permission_tool; /* identity of the equivalent typed tool */
    char *detail;          /* permission detail (resolved path) or NULL */
    char *target;          /* FILE / SERVER/TOOL / NAME / PATH, as typed */
    char *action;          /* memory action; mcp "tools" | "describe" */
    char *value;           /* memory value, ask-user question */
    char *marker;          /* `tny edit --marker` */
    bool json;             /* `--json` was given */
    char *stdin_data;      /* here-doc body or pipe producer output */
    size_t stdin_len;
    char *message; /* refusal text for TNY_INTERCEPT_REFUSED */
} tny_intercept;

/* Classify one `terminal` command. Returns NULL — the common case — when the
 * command must run in the shell. Never fails destructively: an allocation
 * failure also yields NULL. */
tny_intercept *tny_intercept_parse(tools_env *env, const char *command);
void tny_intercept_free(tny_intercept *ic);

/* Run a parsed intercept in-process and return the malloc'd tool result:
 * an `exit: N` line followed by the verb's stdout and stderr, bounded like
 * any other tool result. */
char *tny_intercept_execute(tools_env *env, const tny_intercept *ic);

#endif
