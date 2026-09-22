/* Private wire seam for offline tests. Not a libtny ABI export. */
#ifndef TNYJEV_INTERNAL_H
#define TNYJEV_INTERNAL_H
#include "core/tnyjev.h"
#include "util/util.h"

/* Output buffer must be empty. No credentials in the request body. */
tnyjev_status tnyjev_encode(const tnyjev_request *request, const char *model, buf_t *body);
/* request must have passed encode validation. Result is published atomically. */
tnyjev_status tnyjev_decode(const tnyjev_request *request, const char *body, size_t len,
                            tnyjev_result *result);
#endif
