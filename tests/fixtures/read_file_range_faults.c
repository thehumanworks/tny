/* The integration driver inserts the actual private helper at this marker. */
#include "util/util.h"
#include "core/image.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* PRODUCTION_READ_FILE_RANGE */

typedef struct {
    size_t remaining, calls;
} fault_stream;
static ssize_t read_until_error(void *cookie, char *buffer, size_t size) {
    fault_stream *stream = cookie;
    stream->calls++;
    if (!stream->remaining) {
        errno = EIO;
        return -1;
    }
    size_t n = size < stream->remaining ? size : stream->remaining;
    memset(buffer, 'x', n);
    if (n >= 2) buffer[1] = '\n';
    stream->remaining -= n;
    return (ssize_t)n;
}
static void check(size_t prefix, int64_t offset, int64_t limit, bool success,
                  size_t expected_calls) {
    fault_stream stream = {prefix, 0};
    cookie_io_functions_t callbacks = {.read = read_until_error};
    FILE *file = fopencookie(&stream, "r", callbacks);
    assert(file);
    buf_t out;
    buf_init(&out);
    const char *mime = NULL;
    assert(read_file_range(file, offset, limit, &out, &mime) == success);
    assert(!mime);
    assert(stream.calls == expected_calls);
    if (success) assert(out.len == 2 && memcmp(out.data, "x\n", 2) == 0);
    buf_free(&out);
    fclose(file);
}
int main(void) {
    check(8192, 1, 1, true, 1);  /* The unread tail would fail: do not read it. */
    check(8192, 1, 2, false, 2); /* Selected second line crosses into failure. */
    check(8192, 2, 1, false, 2); /* Skipping a line cannot hide a read failure. */
    check(0, 1, 1, false, 1);    /* First read fails. */
    check(2, 1, 1, false, 2);    /* Error in the same fread is still an error. */
    return 0;
}
