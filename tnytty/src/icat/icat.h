/* icat.h — bundled kitty-graphics image encoder (docs/adr/0003). */
#ifndef TNYTTY_ICAT_H
#define TNYTTY_ICAT_H

#include "util/tt.h"

#include <stddef.h>

/* Encode an image into kitty graphics APC escapes appended to out.
 * PNG bytes pass through (f=100, a=T), chunked base64 with m=1/m=0
 * continuations. Returns 0, or -1 with a static message in *err
 * (non-PNG input, empty input). */
int tt_icat_encode(const unsigned char *data, size_t len, tt_buf *out, const char **err);

/* CLI entry: read path ("-" = stdin), encode, write to stdout followed by
 * a newline. Returns a process exit code. */
int tt_icat_main(const char *path);

#endif
