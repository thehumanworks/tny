/* acp_wire.h — JSON-RPC 2.0 over JSONL framing shared by the ACP client
 * backend and the `tny acp` server (docs/backends/acp.md).
 * One UTF-8 JSON object per line, no embedded newlines, 8 MiB cap. */
#ifndef TNY_ACP_WIRE_H
#define TNY_ACP_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "json/json.h"
#include "util/util.h"

#define ACP_PROTOCOL_VERSION 1
#define ACP_MAX_MSG          (8u * 1024u * 1024u)

/* JSON-RPC error codes (the -32000 range is ours). */
#define ACP_E_PARSE     (-32700)
#define ACP_E_INVALID   (-32600)
#define ACP_E_NO_METHOD (-32601)
#define ACP_E_PARAMS    (-32602)
#define ACP_E_INTERNAL  (-32603)
#define ACP_E_AUTH      (-32000)

/* ---- line framing ---- */

typedef struct {
    buf_t buf;
    bool overflow; /* a line blew the cap: the stream is unusable */
} acp_reader;

void acp_reader_init(acp_reader *r);
void acp_reader_free(acp_reader *r);
void acp_reader_feed(acp_reader *r, const char *data, size_t n);
/* Next complete line (NUL-terminated, malloc'd, no CR/LF). NULL if none. */
char *acp_reader_next(acp_reader *r, size_t *len_out);

/* ---- writing ---- */

/* Write "<json>\n". 0 ok, -1 on a dead pipe. Retries short writes. */
int acp_write_line(int fd, const char *json, size_t len);

/* Message builders (append the JSON object, no newline). `params_json` /
 * `result_json` are raw JSON text; NULL means `{}` (params) or `null`
 * (result). `id_raw` is verbatim id JSON text. Shared by the fd senders
 * below and the WebSocket agent transport (docs/backends/acp.md). */
void acp_fmt_request(buf_t *b, int64_t id, const char *method, const char *params_json);
void acp_fmt_notify(buf_t *b, const char *method, const char *params_json);
void acp_fmt_result(buf_t *b, const char *id_raw, const char *result_json);

/* fd senders: builder + "\n" + write. */
int acp_send_request(int fd, int64_t id, const char *method, const char *params_json);
int acp_send_notify(int fd, const char *method, const char *params_json);
int acp_send_result(int fd, const char *id_raw, const char *result_json);
int acp_send_error(int fd, const char *id_raw, int code, const char *msg);

/* ---- small helpers ---- */

/* Verbatim JSON text of a message's "id" (malloc'd; "null" when absent). */
char *acp_id_text(yyjson_val *msg);
/* Numeric id, or -1 when the id is absent or not an integer. */
int64_t acp_id_num(yyjson_val *msg);

/* Flatten a ContentBlock[] into plain text. Returns false and sets *bad to
 * the offending block type when a block we cannot represent is present. */
bool acp_blocks_to_text(yyjson_val *arr, buf_t *out, const char **bad);

/* Append a ContentBlock text object: {"type":"text","text":"…"} */
void acp_append_text_block(buf_t *b, const char *text, size_t len);

#endif
