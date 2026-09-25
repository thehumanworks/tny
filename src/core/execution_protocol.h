/* Pure admission kernel and strict JSON envelope for private execution RPC. */
#ifndef TNY_EXECUTION_PROTOCOL_H
#define TNY_EXECUTION_PROTOCOL_H
#include "json/json.h"
#include <stdbool.h>
#include <stdint.h>
#define TNY_EXEC_FRAME_MAX        (8u * 1024u * 1024u)
#define TNY_EXEC_PROTOCOL_VERSION 1
/* Values are explicit for exhaustive external checking of the compiled predicate. */
typedef enum {
    TNY_EXEC_WAIT_START = 0,
    TNY_EXEC_WAIT_RESULT = 1,
    TNY_EXEC_WAIT_REPLY = 2,
    TNY_EXEC_DONE = 3
} tny_exec_phase;
typedef enum {
    TNY_EXEC_INVALID = 0,
    TNY_EXEC_EXECUTE = 1,
    TNY_EXEC_CONTROL = 2,
    TNY_EXEC_EVENT = 3,
    TNY_EXEC_PROMPT = 4,
    TNY_EXEC_ASK = 5,
    TNY_EXEC_STATE = 6,
    TNY_EXEC_RESULT = 7
} tny_exec_kind;
bool tny_exec_protocol_admit(tny_exec_phase phase, tny_exec_kind kind, uint64_t id,
                             uint64_t expected);
/* No NUL strings, duplicate keys, excessive depth, or unbounded member counts. */
bool tny_exec_json_valid(yyjson_val *root);
/* Unknown members/notifications/errors/invalid ids are rejected. */
tny_exec_kind tny_exec_envelope(yyjson_val *root, uint64_t *id, yyjson_val **body);
char *tny_exec_message(uint64_t id, tny_exec_kind kind, yyjson_val *body);
#endif
