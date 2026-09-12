/* cmd_control.h — standalone session-control CLI verbs (ADR 0058) and the
 * private data-returning control primitive they share (docs/adr/0096). */
#ifndef TNY_CMD_CONTROL_H
#define TNY_CMD_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

/* `json` carries the already-parsed leading global --json flag. Both
 * commands also accept --json after the subcommand. */
int cmd_ask_user(bool json, int argc, char **argv);
int cmd_image(bool json, int argc, char **argv);

/* ---- private control primitive ----
 * One correlated tool-role request over TNY_SESSION_SOCK, returning data. It
 * prints nothing and exits nothing on any platform, including the WebAssembly
 * branch, so a caller that wants a status inside its own result (a generated
 * image preview) is not forced through command stdout. The CLI wrappers above
 * keep their existing messages and exit codes. */

typedef enum {
    TNY_CONTROL_OP_ASK_USER = 1,
    TNY_CONTROL_OP_IMAGE_ATTACH,
    TNY_CONTROL_OP_IMAGE_PREVIEW
} tny_control_op;

typedef enum {
    TNY_CONTROL_EXCHANGE_OK = 0, /* a correlated reply arrived; read the reply */
    TNY_CONTROL_EXCHANGE_NO_SOCKET,
    TNY_CONTROL_EXCHANGE_UNSUPPORTED, /* this runtime has no session socket */
    TNY_CONTROL_EXCHANGE_OOM,
    TNY_CONTROL_EXCHANGE_TOO_LARGE,
    TNY_CONTROL_EXCHANGE_CONNECT_FAILED,
    TNY_CONTROL_EXCHANGE_WRITE_FAILED,
    TNY_CONTROL_EXCHANGE_INTERRUPTED,
    /* The request was written but no reply arrived. Delivery is unknown: it is
     * never retried, never reported as success and never queued twice. */
    TNY_CONTROL_EXCHANGE_CLOSED
} tny_control_exchange;

typedef struct {
    bool ok;          /* the receiver's own ok flag */
    char *id;         /* correlation id this exchange used */
    char *answer;     /* ask_user answer, or NULL */
    char *error;      /* safe error text from the receiver, or NULL */
    char *status;     /* optional: image_preview admission status, or NULL */
    char *error_code; /* optional: safe machine-readable code, or NULL */
} tny_control_reply;

/* `payload` is the question or path. `expected_sha256` is required by
 * TNY_CONTROL_OP_IMAGE_PREVIEW and ignored otherwise. expected_bytes == 0
 * omits the optional length; a positive length pins the exact captured bytes.
 * *reply is always
 * initialized; free it with tny_control_reply_free. */
tny_control_exchange tny_control_request(tny_control_op op, const char *payload,
                                         const char *expected_sha256, uint64_t expected_bytes,
                                         tny_control_reply *reply);
void tny_control_reply_free(tny_control_reply *reply);

#endif
