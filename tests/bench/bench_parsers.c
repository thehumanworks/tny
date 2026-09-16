/* Identical C-facing parser workload for pre-migration and candidate sources.
 * Build with bench_parsers.py, which instruments both allocation boundaries
 * identically while retaining release optimization and recording all flags. */
#include "net/net.h"
#include "backends/openai/openai.h"
#include "util/alloc.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#define CALL_COUNT 32
#define ARG_BYTES  192
#define FNV_OFFSET UINT64_C(14695981039346656037)
#define FNV_PRIME  UINT64_C(1099511628211)

typedef struct {
    uint64_t hash;
    uint64_t events;
} observation;

typedef struct {
    yyjson_doc **docs;
    size_t count;
    size_t bytes;
} tool_corpus;

static void observe(const char *data, size_t len, void *ud) {
    observation *o = ud;
    for (size_t i = 0; i < len; i++) {
        o->hash ^= (unsigned char)data[i];
        o->hash *= FNV_PRIME;
    }
    o->hash ^= (uint64_t)len;
    o->hash *= FNV_PRIME;
    o->events++;
}

static void observe_frame(uint8_t flags, const char *data, size_t len, void *ud) {
    observation *o = ud;
    o->hash ^= flags;
    o->hash *= FNV_PRIME;
    observe(data, len, ud);
}

static size_t fragment_size(const char *pattern, size_t remaining, uint32_t *seed) {
    if (strcmp(pattern, "whole") == 0) return remaining;
    if (strcmp(pattern, "byte") == 0) return 1;
    *seed ^= *seed << 13;
    *seed ^= *seed >> 17;
    *seed ^= *seed << 5;
    size_t n = 1 + *seed % 97u;
    return n < remaining ? n : remaining;
}

static bool make_wire(const char *mode, buf_t *wire) {
    char payload[512];
    for (int i = 0; i < 32; i++) {
        int n = snprintf(payload, sizeof payload,
                         "{\"type\":\"response.output_text.delta\",\"index\":%d,"
                         "\"delta\":\"stream payload with UTF-8: \xc3\xa9 \xf0\x9f\x8c\x8a; "
                         "the parser must preserve every byte and event boundary.\"}",
                         i);
        if (n < 0 || (size_t)n >= sizeof payload) return false;
        if (strcmp(mode, "sse") == 0) {
            buf_appends(wire, ": keepalive\r\nevent: ignored\r\ndata: ");
            buf_append(wire, payload, (size_t)n);
            buf_appends(wire, "\r\ndata: continuation\r\n\r\n");
        } else {
            connect_frame_encode(wire, 0, "", 0);
            connect_frame_encode(wire, 0, payload, (size_t)n);
        }
    }
    if (strcmp(mode, "sse") == 0) buf_appends(wire, "data: [DONE]");
    else connect_frame_encode(wire, CONNECT_FLAG_END, "{\"metadata\":{}}", 15);
    return !buf_oom(wire);
}

static void free_tools(tool_corpus *corpus) {
    for (size_t i = 0; i < corpus->count; i++) yyjson_doc_free(corpus->docs[i]);
    free(corpus->docs);
    memset(corpus, 0, sizeof *corpus);
}

static bool make_tools(const char *pattern, tool_corpus *corpus) {
    char argument[ARG_BYTES + 1];
    for (size_t i = 0; i < ARG_BYTES; i++) argument[i] = (char)('a' + i % 26);
    argument[ARG_BYTES] = 0;
    corpus->docs = calloc(CALL_COUNT * ARG_BYTES, sizeof *corpus->docs);
    if (!corpus->docs) return false;
    for (int call = 0; call < CALL_COUNT; call++) {
        size_t at = 0;
        uint32_t seed = 0x12345678u;
        while (at < ARG_BYTES) {
            size_t count = fragment_size(pattern, ARG_BYTES - at, &seed);
            char part[ARG_BYTES + 1];
            memcpy(part, argument + at, count);
            part[count] = 0;
            buf_t json;
            buf_init(&json);
            /* Reused indices deliberately exercise id-first attribution. */
            buf_appendf(&json, "[{\"id\":\"call_%d\",\"index\":%d,\"function\":{", call, call % 4);
            if (at == 0) buf_appends(&json, "\"name\":\"list_files\",");
            buf_appends(&json, "\"arguments\":");
            jescape(&json, part);
            buf_appends(&json, "}}]");
            yyjson_doc *doc = buf_oom(&json) ? NULL : jparse(json.data, json.len);
            corpus->bytes += json.len;
            buf_free(&json);
            if (!doc) return false;
            corpus->docs[corpus->count++] = doc;
            at += count;
        }
    }
    return true;
}

static bool run_wire(const char *mode, const char *pattern, const buf_t *wire, observation *out) {
    size_t at = 0;
    uint32_t seed = 0x12345678u;
    if (strcmp(mode, "sse") == 0) {
        sse_parser parser;
        sse_parser_init(&parser);
        while (at < wire->len) {
            size_t n = fragment_size(pattern, wire->len - at, &seed);
            sse_feed(&parser, wire->data + at, n, observe, out);
            at += n;
        }
        sse_flush(&parser, observe, out);
        sse_parser_free(&parser);
    } else {
        connect_decoder parser;
        connect_decoder_init(&parser);
        while (at < wire->len) {
            size_t n = fragment_size(pattern, wire->len - at, &seed);
            if (connect_decoder_feed(&parser, wire->data + at, n, observe_frame, out) != 0) {
                connect_decoder_free(&parser);
                return false;
            }
            at += n;
        }
        connect_decoder_free(&parser);
    }
    return !tny_alloc_scope_failed();
}

static bool run_tools(const tool_corpus *corpus, observation *out) {
    oa_callset calls = {0};
    for (size_t i = 0; i < corpus->count; i++)
        oa_calls_feed(&calls, yyjson_doc_get_root(corpus->docs[i]));
    bool valid = calls.n == CALL_COUNT && !tny_alloc_scope_failed();
    for (int i = 0; i < calls.n && valid; i++) {
        char expected[32];
        snprintf(expected, sizeof expected, "call_%d", i);
        const oa_call *call = &calls.calls[i];
        valid = call->id && strcmp(call->id, expected) == 0 && call->name &&
                strcmp(call->name, "list_files") == 0 && call->args.len == ARG_BYTES;
        if (valid) {
            observe(call->id, strlen(call->id), out);
            observe(call->name, strlen(call->name), out);
            observe(call->args.data, call->args.len, out);
        }
    }
    oa_calls_reset(&calls);
    return valid;
}

static uint64_t nanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc != 4 ||
        (strcmp(argv[1], "sse") && strcmp(argv[1], "connect") && strcmp(argv[1], "tools")) ||
        (strcmp(argv[2], "whole") && strcmp(argv[2], "byte") && strcmp(argv[2], "split"))) {
        fputs("usage: bench-parsers sse|connect|tools whole|byte|split ITERATIONS\n", stderr);
        return 2;
    }
    errno = 0;
    char *end = NULL;
    unsigned long iterations = strtoul(argv[3], &end, 10);
    if (errno || !end || *end || iterations < 1 || iterations > 1000000) return 2;
    buf_t wire;
    buf_init(&wire);
    tool_corpus corpus = {0};
    bool tools = strcmp(argv[1], "tools") == 0;
    bool ok = tools ? make_tools(argv[2], &corpus) : make_wire(argv[1], &wire);
    observation out = {FNV_OFFSET, 0};
    tny_alloc_scope_begin("parser_benchmark");
    uint64_t start = nanoseconds();
    for (unsigned long i = 0; i < iterations && ok; i++)
        ok = tools ? run_tools(&corpus, &out) : run_wire(argv[1], argv[2], &wire, &out);
    uint64_t finish = nanoseconds();
    size_t allocations = tny_alloc_test_scope_count();
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) ok = false;
    uint64_t peak = ok ? (uint64_t)usage.ru_maxrss : 0;
#ifndef __APPLE__
    peak *= 1024;
#endif
    uint64_t expected = (uint64_t)iterations * (tools ? CALL_COUNT * 3u : 33u);
    ok = ok && start && finish >= start && out.events == expected;
    if (ok)
        printf("{\"mode\":\"%s\",\"fragmentation\":\"%s\",\"iterations\":%lu,"
               "\"nanoseconds\":%" PRIu64 ",\"input_bytes\":%" PRIu64 ",\"events\":%" PRIu64
               ",\"checksum\":\"%016" PRIx64 "\",\"allocations\":%zu,\"peak_rss_bytes\":%" PRIu64
               "}\n",
               argv[1], argv[2], iterations, finish - start,
               (uint64_t)iterations * (tools ? corpus.bytes : wire.len), out.events, out.hash,
               allocations, peak);
    else fputs("parser benchmark failed its behavior/allocation oracle\n", stderr);
    free_tools(&corpus);
    buf_free(&wire);
    return ok ? 0 : 1;
}
