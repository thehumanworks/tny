/* mcp_priv.h — shared MCP client types (stdio + Streamable HTTP). */
#ifndef TNY_MCP_PRIV_H
#define TNY_MCP_PRIV_H

#include "mcp/mcp.h"
#include "net/net.h"

#include <sys/types.h>

#define MCP_MAX_SERVERS    16
#define MCP_TIMEOUT_MS     30000
#define MCP_HTTP_MAX_BODY  (16U * 1024U * 1024U)
#define MCP_MAX_HEADERS    32
#define MCP_MODERN_VERSION "2026-07-28"
#define MCP_LEGACY_VERSION "2025-06-18"

typedef enum { MCP_TRANSPORT_STDIO = 0, MCP_TRANSPORT_HTTP } mcp_transport;
typedef enum { MCP_ERA_LEGACY = 0, MCP_ERA_MODERN } mcp_era;

typedef struct {
    mcp_transport transport;
    char **argv;
    char *url;
    char **headers;
    size_t nheaders;
} mcp_conf;

/* One live MCP connection. HTTP opens one socket/fetch per POST; its logical
 * connection is the copied configuration plus optional legacy session id. */
typedef struct {
    mcp_transport transport;
    mcp_era era;
    pid_t pid;
    int in_fd;
    int out_fd;
    buf_t rbuf;
    int next_id;
    yyjson_doc *tools;
    char *url;
    char path[1024];
    char **headers;
    size_t nheaders;
    char *session_id; /* opaque legacy Mcp-Session-Id; never logged */
    char protocol_version[16];
    char last_error[256]; /* sanitized: never contains configured headers */
} mcp_conn;

void mcp_secret_free(char *s);
void mcp_header_lines_free(char **headers, size_t nheaders);
void mcp_conf_free(mcp_conf *conf);
void mcp_conn_close(mcp_conn *c);

yyjson_doc *mcp_rpc_http(mcp_conn *c, const char *method, const char *name, const char *params_json,
                         bool capture_session, int *status_out);
int mcp_notify_http(mcp_conn *c, const char *method);
void mcp_http_status_error(mcp_conn *c, int status);
int mcp_conn_open_http(mcp_conn *c, mcp_conf *conf);

#endif
