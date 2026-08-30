/* protocol.h — bounded, split-safe parsing for the broker's existing HTTP
 * surface and its versioned terminal snapshot content type. */
#ifndef TNYTTY_BROKER_PROTOCOL_H
#define TNYTTY_BROKER_PROTOCOL_H

#include "util/tt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TT_BROKER_RESPONSE_MAX (32u * 1024u * 1024u)

typedef struct {
    tt_buf bytes;
    size_t header_len;
    size_t content_len;
    int status;
    bool complete;
} tt_http_response_parser;

void tt_http_response_parser_init(tt_http_response_parser *p);
void tt_http_response_parser_free(tt_http_response_parser *p);
/* Feed any fragment. Returns 1 when complete, 0 when more is needed, or
 * -1 with errno set for malformed/oversized input. */
int tt_http_response_parser_feed(tt_http_response_parser *p, const void *bytes, size_t len);
const unsigned char *tt_http_response_body(const tt_http_response_parser *p, size_t *len);

#endif
