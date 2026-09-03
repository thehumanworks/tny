/* cli.h — command dispatch. Exit codes: 0 ok, 1 startup/config, 2 run failed,
 * 130 interrupted (docs/cli.md). */
#ifndef TNY_CLI_H
#define TNY_CLI_H

#include "core/config.h"

#include <stdint.h>
#include <stdio.h>

/* Leading global flags, parsed before the subcommand. */
typedef struct {
    const char *backend;                  /* --backend */
    const char *cwd;                      /* --cwd */
    const char *model;                    /* --model */
    const char *effort;                   /* --effort | --reasoning-effort */
    const char *system_prompt;            /* --system-prompt */
    const char *task;                     /* --task NAME */
    const char *perm_mode;                /* --permission-mode | --yolo | --auto */
    const char *max_steps;                /* --max-steps N|unlimited (0 = no cap) */
    const char *max_extension_iterations; /* 0/unlimited = no cap */
    bool no_extensions;                   /* --no-extensions */
    bool fast;                            /* --fast (providers with TNY_CAP_FAST) */
    const char **add_dirs;                /* --add-dir, repeatable */
    int n_add_dirs;
    bool json;           /* --json */
    const char *color;   /* --color auto|always|never (--no-color = never) */
    const char *ssh;     /* --ssh user@host[:port] */
    const char *ssh_cwd; /* --ssh-cwd DIR (remote) */
    bool ephemeral;      /* --ephemeral | --no-save */
    bool resume_picker;  /* -r */
    bool resume_last;    /* -c / --continue */
    const char *resume;  /* --resume value */
    /* backend-specific */
    const char *bridge_bin;
    const char *codex_bin;   /* login helper (docs/adr/0065) */
    const char **agent_argv; /* --agent CMD -- args…, NULL-terminated */
    const char *base_url;
    const char *api_key_env;
    const char *wire_api; /* --wire-api responses|chat */
} cli_globals;

/* Internal Cursor management stream seam. Kept here so unit tests can inject
 * a failing FILE without changing stdout or the public embedding ABI. */
typedef struct {
    FILE *stream;
    size_t total;
} cursor_artifact_output;

int cursor_cli_artifact_frame(uint8_t flags, const char *payload, size_t len, void *ud, char *err,
                              size_t errlen);

/* Parse leading globals; returns index of the subcommand in argv or -1 on
 * error (message already printed). */
int cli_parse_globals(int argc, char **argv, cli_globals *g);

/* --ssh TARGET: open the remote tool runtime on ctx (docs/adr/0022). Prints
 * its own error; 0 ok. Shared by cli_make_ctx and the TUI /ssh command. */
int cli_ssh_attach(tny_ctx *ctx, const char *target, const char *remote_cwd);

/* Build ctx from globals (loads settings). NULL = startup error. */
tny_ctx *cli_make_ctx(const cli_globals *g);

/* "session <id> is still running (pid N)" + watch/stop/steer hints, on
 * stderr. Shared by every lock-refusal path (docs/adr/0031 decision 7). */
void cli_print_still_running(tny_ctx *ctx, const char *id);

/* Commands. Each returns the process exit code. */
int cmd_ask(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
/* Stateless and config-free; dispatched before cli_make_ctx. */
int cmd_edit(const cli_globals *g, int argc, char **argv);
int cmd_resume(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_sessions(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_session(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_status(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_status_ephemeral(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_doctor(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_models(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_permissions(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_workspace(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_backends(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_provider(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_usage(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_cursor(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_mcp(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_setup(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_tasks(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_task(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
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
