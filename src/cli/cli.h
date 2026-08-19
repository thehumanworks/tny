/* cli.h — command dispatch. Exit codes: 0 ok, 1 startup/config, 2 run failed,
 * 130 interrupted (docs/cli.md). */
#ifndef TNY_CLI_H
#define TNY_CLI_H

#include "core/config.h"

/* Leading global flags, parsed before the subcommand. */
typedef struct {
    const char *backend;     /* --backend */
    const char *cwd;         /* --cwd */
    const char *model;       /* --model */
    const char *perm_mode;   /* --permission-mode | --yolo | --auto */
    const char **add_dirs;   /* --add-dir, repeatable */
    int         n_add_dirs;
    bool        json;        /* --json */
    bool        resume_picker;   /* -r */
    bool        resume_last;     /* -c / --continue */
    const char *resume;      /* --resume value */
    /* backend-specific */
    const char *bridge_bin;
    const char *codex_ws;
    const char *codex_bin;
    const char *ws_token_file;
    const char **agent_argv; /* --agent CMD -- args…, NULL-terminated */
    const char *base_url;
    const char *api_key_env;
} cli_globals;

/* Parse leading globals; returns index of the subcommand in argv or -1 on
 * error (message already printed). */
int cli_parse_globals(int argc, char **argv, cli_globals *g);

/* Build ctx from globals (loads settings). NULL = startup error. */
tny_ctx *cli_make_ctx(const cli_globals *g);

/* Commands. Each returns the process exit code. */
int cmd_ask(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_resume(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_sessions(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_session(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_status(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_doctor(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_models(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_permissions(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_workspace(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_backends(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_usage(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_setup(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_login(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_logout(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_acp_server(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_tui(tny_ctx *ctx, const cli_globals *g);
/* Interactive shell starting on an existing session (NULL = picker/new). */
int cmd_tui_resume(tny_ctx *ctx, const cli_globals *g, const char *session_id);

/* help.c */
void help_root(void);
/* Returns true if it printed help for the named command. */
bool help_for(const char *command);

#endif
