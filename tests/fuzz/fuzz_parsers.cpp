/* Portable parser corpus smoke and libFuzzer entry point. The same bytes are
 * fed whole, one byte at a time, and in deterministic fragments. */
#include "json/ownership.hpp"
#include "backends/openai/stream_decode.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
struct digest {
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t count = 0;
    void bytes(const char *data, size_t len) {
        for (size_t i = 0; i < len; ++i)
            hash = (hash ^ static_cast<unsigned char>(data[i])) * UINT64_C(1099511628211);
    }
    void number(uint64_t n) {
        for (int i = 0; i < 8; ++i) {
            char byte = static_cast<char>(n);
            bytes(&byte, 1);
            n >>= 8;
        }
    }
    bool operator==(const digest &) const = default;
};
struct stream {
    oa_decoder decoder{};
    oa_callset calls{};
    digest result;
    bool chat;
    int status = TNY_PARSE_OK;
    explicit stream(bool chat_wire) : chat(chat_wire) {}
    ~stream() {
        oa_decoder_reset(&decoder);
        oa_calls_reset(&calls);
    }
    stream(const stream &) = delete;
    stream &operator=(const stream &) = delete;
};
int event(const oa_decoded_event *e, void *ud) {
    auto &d = *static_cast<digest *>(ud);
    ++d.count;
    d.number(e->kind);
    d.number(e->stop);
    d.number(e->ok);
    if (e->text) d.bytes(e->text, e->len ? e->len : std::strlen(e->text));
    return TNY_PARSE_OK;
}
void sse_event(const char *data, size_t len, void *ud) {
    auto &s = *static_cast<stream *>(ud);
    if (s.status == TNY_PARSE_OOM) return;
    s.result.number(len);
    s.result.bytes(data, len);
    int rc = oa_decoder_feed(&s.decoder, &s.calls, s.chat, true, data, len, event, &s.result);
    if (rc == TNY_PARSE_OOM) s.status = rc;
}
void frame(uint8_t flags, const char *data, size_t len, void *ud) {
    auto &d = *static_cast<digest *>(ud);
    ++d.count;
    d.number(flags);
    d.number(len);
    d.bytes(data, len);
}
size_t portion(size_t left, size_t position, int mode) {
    return mode == 0 ? left : mode == 1 ? 1 : std::min(left, 1 + position % 23);
}
digest sse_run(const char *data, size_t len, int mode, bool chat) {
    stream s(chat);
    sse_parser parser;
    sse_parser_init(&parser);
    for (size_t pos = 0; pos < len;) {
        size_t take = portion(len - pos, pos, mode);
        int rc = sse_feed(&parser, data + pos, take, sse_event, &s);
        pos += take;
        if (rc) {
            s.status = rc;
            break;
        }
    }
    int rc = sse_flush(&parser, sse_event, &s);
    if (rc) s.status = rc;
    sse_parser_free(&parser);
    s.result.number(static_cast<uint64_t>(s.status));
    for (int i = 0; i < s.calls.n; ++i) {
        const auto &c = s.calls.calls[i];
        if (c.id) s.result.bytes(c.id, std::strlen(c.id));
        if (c.name) s.result.bytes(c.name, std::strlen(c.name));
        if (c.args.data) s.result.bytes(c.args.data, c.args.len);
    }
    char *extras = nullptr;
    rc = oa_decoder_extras(&s.decoder, &extras);
    tny::c_string owner(extras);
    s.result.number(static_cast<uint64_t>(rc));
    if (extras) s.result.bytes(extras, std::strlen(extras));
    return s.result;
}
digest connect_run(const char *data, size_t len, int mode) {
    digest d;
    connect_decoder decoder;
    connect_decoder_init(&decoder);
    int rc = TNY_PARSE_OK;
    for (size_t pos = 0; pos < len;) {
        size_t take = portion(len - pos, pos, mode);
        rc = connect_decoder_feed(&decoder, data + pos, take, frame, &d);
        pos += take;
        if (rc) break;
    }
    d.number(static_cast<uint64_t>(rc));
    d.number(static_cast<uint64_t>(connect_decoder_finish(&decoder)));
    connect_decoder_free(&decoder);
    return d;
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const char *bytes = reinterpret_cast<const char *>(data);
    for (bool chat : {false, true}) {
        digest whole = sse_run(bytes, size, 0, chat);
        assert(whole == sse_run(bytes, size, 1, chat));
        assert(whole == sse_run(bytes, size, 2, chat));
        stream s(chat);
        (void)oa_decoder_feed(&s.decoder, &s.calls, chat, true, bytes, size, event, &s.result);
    }
    digest whole = connect_run(bytes, size, 0);
    assert(whole == connect_run(bytes, size, 1));
    assert(whole == connect_run(bytes, size, 2));
    return 0;
}
#ifdef TNY_FUZZ_STANDALONE
int main(int argc, char **argv) {
    static const char *const seeds[] = {
        "",
        "data: [DONE]\n\n",
        "data: DONE\r\n\r\n",
        ": comment\n\ndata: h\xc3\xa9\n",
        "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n",
        ("data: {\"type\":\"response.output_text.delta\",\"delta\":\"hi\"}\n\ndata: "
         "{\"type\":\"response.completed\"}\n\n")};
    for (auto *seed : seeds)
        LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t *>(seed), std::strlen(seed));
    for (int i = 1; i < argc; ++i) {
        FILE *file = std::fopen(argv[i], "rb");
        if (!file) return 1;
        tny::bytes bytes;
        unsigned char chunk[4096];
        size_t n;
        while ((n = std::fread(chunk, 1, sizeof chunk, file)))
            bytes.insert(bytes.end(), chunk, chunk + n);
        bool failed = std::ferror(file);
        std::fclose(file);
        if (failed) return 1;
        LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    }
    std::printf("parser corpus smoke: %d files plus built-in seeds\n", argc - 1);
    return 0;
}
#endif
