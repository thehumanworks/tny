/* Private synchronous provider decoder. No request/tool/retry scheduling. */
#ifndef TNY_OPENAI_STREAM_DECODE_H
#define TNY_OPENAI_STREAM_DECODE_H
#include "backends/openai/toolcalls.h"
#include "core/events.h"
#include "net/net.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *owner; /* zero initialize; owns retained reasoning, never copy */
    int status;  /* sticky OOM; malformed individual events are not sticky */
} oa_decoder;
typedef enum {
    OA_DECODE_TEXT,
    OA_DECODE_THINKING,
    OA_DECODE_USAGE,
    OA_DECODE_ERROR,
    OA_DECODE_FINISH,
    OA_DECODE_DONE,
    OA_DECODE_HOSTED_START,
    OA_DECODE_HOSTED_END
} oa_decoded_kind;
typedef struct {
    oa_decoded_kind kind;
    const char *text; /* borrowed only during callback; may contain NUL */
    size_t len;
    yyjson_val *value; /* borrowed error/usage, only during callback */
    tny_stop_reason stop;
    bool ok;
    int64_t input_tokens, output_tokens, cached_tokens, cache_write_tokens;
    unsigned usage_fields; /* bits 1, 2, 4, 8: present numeric values */
} oa_decoded_event;
/* Return nonzero to stop dispatch, e.g. when a C consumer cannot retain text.
 * Decoder never invokes callbacks until its own allocation work succeeded. */
typedef int (*oa_decoded_cb)(const oa_decoded_event *event, void *ud);
int oa_decoder_feed(oa_decoder *decoder, oa_callset *calls, bool chat, bool hosted,
                    const char *data, size_t len, oa_decoded_cb cb, void *ud);
void oa_decoder_reset(oa_decoder *decoder);
/* Allocates caller-owned JSON (free). NULL + OK means no extras. */
int oa_decoder_extras(oa_decoder *decoder, char **out);
#ifdef __cplusplus
}
#endif
#endif
