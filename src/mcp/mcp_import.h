/* mcp_import.h — opt-in merge of MCP servers from other harnesses
 * (docs/adr/0051). Off by default; never reads foreign files unless
 * settings.json names the source in mcp.import_from. */
#ifndef TNY_MCP_IMPORT_H
#define TNY_MCP_IMPORT_H

#include "core/config.h"
#include "json/json.h"

#define MCP_IMPORT_MAX_SERVERS 16
#define MCP_IMPORT_MAX_ARGV    32
#define MCP_IMPORT_MAX_ENV     16
#define MCP_IMPORT_MAX_NOTICES 16

typedef enum {
    MCP_SRC_TNY = 0,
    MCP_SRC_CODEX,
    MCP_SRC_CLAUDE,
    MCP_SRC_GROK,
    MCP_SRC_CURSOR
} mcp_source_id;

typedef struct {
    char *key;
    char *value;
} mcp_env_pair;

typedef struct {
    char *name; /* native identity used in mcp_select_tool */
    mcp_source_id source;
    char *origin;    /* tny | codex | claude | grok | cursor-agent */
    char *scope;     /* user | project */
    char *transport; /* stdio | http | sse | unknown */
    char *url;       /* retained for #87; never printed */
    char *argv[MCP_IMPORT_MAX_ARGV];
    int argc;
    mcp_env_pair env[MCP_IMPORT_MAX_ENV];
    int nenv;
    char *cwd;         /* optional stdio working directory */
    bool skipped;      /* remote/SSE/ws — listed, not spawned */
    char *skip_reason; /* short, no secrets */
} mcp_imported_server;

typedef struct {
    mcp_imported_server servers[MCP_IMPORT_MAX_SERVERS];
    int nservers;
    char *notices[MCP_IMPORT_MAX_NOTICES];
    int nnotices;
    bool loaded;
} mcp_catalog;

/* Resolve ~/.tny/mcp.json plus any opted-in foreign sources. Never reads a
 * foreign file unless mcp.import_from names it. mcp_disabled / library_mode
 * yield an empty catalog. Caller mcp_catalog_free's. */
mcp_catalog *mcp_catalog_load(struct tny_ctx *ctx);
void mcp_catalog_free(mcp_catalog *cat);

const mcp_imported_server *mcp_catalog_find(const mcp_catalog *cat, const char *name);
const char *mcp_source_name(mcp_source_id id);
bool mcp_transport_supported(const char *transport);

#endif
