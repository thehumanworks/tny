/* Shared portable corpus/fault checks and Linux libFuzzer entry point. */
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
extern "C" {
#include "net/net.h"
#include "backends/openai/parsers.h"
#include "util/alloc.h"
}

namespace {
void check(bool ok, const char *why) {
    if (!ok) {
        std::fprintf(stderr, "parser check failed: %s\n", why);
        std::abort();
    }
}
struct capture {
    uint64_t hash = 14695981039346656037ULL;
    size_t count = 0;
    void bytes(const char *data, size_t len) {
        for (size_t i = 0; i < len; i++)
            hash = (hash ^ static_cast<unsigned char>(data[i])) * 1099511628211ULL;
    }
};
void event(const char *data, size_t len, void *ud) {
    auto &out = *static_cast<capture *>(ud);
    out.count++;
    out.bytes(data, len);
    out.bytes("\0", 1);
}
void frame(uint8_t flags, const char *data, size_t len, void *ud) {
    auto &out = *static_cast<capture *>(ud);
    out.bytes(reinterpret_cast<const char *>(&flags), 1);
    event(data, len, ud);
}
void decoded(oa_decoded_kind kind, yyjson_val *, const char *bytes, size_t len, void *ud) {
    auto &out = *static_cast<capture *>(ud);
    auto k = static_cast<char>(kind);
    out.bytes(&k, 1);
    event(bytes, len, ud);
}
struct decoding {
    oa_callset calls{};
    capture out;
    bool chat = true;
    int status = 0;
    decoding() = default;
    decoding(const decoding &) = delete;
    decoding &operator=(const decoding &) = delete;
    ~decoding() { oa_calls_reset(&calls); }
};
void decode_sse(const char *data, size_t len, void *ud) {
    auto &d = *static_cast<decoding *>(ud);
    if (d.status < 0) return;
    int rc = oa_decode_event(d.chat, data, len, &d.calls, decoded, &d.out);
    if (rc < 0) d.status = rc;
}
struct result {
    uint64_t hash;
    size_t count;
    int status;
    bool pending;
    bool operator==(const result &) const = default;
};
result consume(std::string_view wire, size_t chunk, unsigned mode) {
    sse_parser sse{};
    connect_decoder connect{};
    capture frames;
    decoding d;
    d.chat = mode != 2;
    int status = 0;
    for (size_t offset = 0; offset < wire.size() && !status;
         offset += std::min(chunk, wire.size() - offset)) {
        auto piece = wire.substr(offset, chunk);
        if (mode == 0)
            status = connect_decoder_feed(&connect, piece.data(), piece.size(), frame, &frames);
        else status = sse_feed(&sse, piece.data(), piece.size(), decode_sse, &d);
    }
    if (mode != 0 && !status) status = sse_flush(&sse, decode_sse, &d);
    if (d.status < 0) status = d.status;
    for (int i = 0; i < d.calls.n; i++) {
        const auto &c = d.calls.calls[i];
        if (c.id) d.out.bytes(c.id, std::strlen(c.id));
        if (c.name) d.out.bytes(c.name, std::strlen(c.name));
        d.out.bytes(c.args.data, c.args.len);
    }
    auto out = mode == 0 ? frames : d.out;
    bool pending = connect_decoder_pending(&connect);
    sse_parser_free(&sse);
    connect_decoder_free(&connect);
    return {out.hash, out.count, status, pending};
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!size) return 0;
    tny_alloc_scope_begin("parser-fuzz");
    unsigned mode = data[0] % 3;
    std::string_view wire(reinterpret_cast<const char *>(data + 1), size - 1);
    auto whole = consume(wire, std::max(size, size_t(1)), mode);
    auto bytes = consume(wire, 1, mode);
    auto fragmented = consume(wire, 1 + data[0] % 31, mode);
    check(whole == bytes && bytes == fragmented, "fragmentation changed decoded output");
    return 0;
}

#ifdef TNY_PARSER_STANDALONE
namespace {
constexpr const char *chat =
    "data: "
    "{\"choices\":[{\"delta\":{\"content\":\"h\xc3\xa9\",\"tool_calls\":[{\"index\":0,\"id\":"
    "\"call_abcdefghijklmnop\",\"function\":{\"name\":\"terminal\",\"arguments\":\"{"
    "\\\"command\\\":\\\"echo hello\\\"}\"}}]}}]}\r\n\r\ndata: [DONE]\n\n";
constexpr const char *responses =
    "data: "
    "{\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"function_"
    "call\",\"call_id\":\"call_abcdefghijklmnop\",\"name\":\"terminal\"}}\n\ndata: "
    "{\"type\":\"response.function_call_arguments.delta\",\"output_index\":0,\"delta\":\"{"
    "\\\"command\\\":\\\"echo hello\\\"}\"}\n\ndata: "
    "{\"type\":\"response.completed\",\"response\":{\"usage\":{\"input_tokens\":4}}}\n\n";
void split_positions() {
    const char *sse_wire =
        ":keepalive\r\ndata: h\xc3\xa9\r\ndata: \xf0\x9f\x90\x95\r\n\r\ndata: tail\r";
    for (size_t split = 0; split <= std::strlen(sse_wire); split++) {
        sse_parser p{};
        capture actual, expected;
        event("h\xc3\xa9\n\xf0\x9f\x90\x95", 8, &expected);
        event("tail", 4, &expected);
        check(sse_feed(&p, sse_wire, split, event, &actual) == 0, "SSE split prefix");
        check(sse_feed(&p, sse_wire + split, std::strlen(sse_wire) - split, event, &actual) == 0,
              "SSE split suffix");
        check(sse_flush(&p, event, &actual) == 0, "SSE flush");
        check(actual.count == 2 && actual.hash == expected.hash, "SSE CRLF/multiline/UTF8 bytes");
        sse_parser_free(&p);
    }
    const char wire[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 'o', 'k', 2, 0, 0, 0, 0, 0, 0, 0};
    auto expected = consume(std::string_view(wire, sizeof wire), sizeof wire, 0);
    check(expected.count == 2 && expected.pending, "Connect keepalive/trailer/truncation");
    for (size_t split = 0; split <= sizeof wire; split++) {
        connect_decoder p{};
        capture actual;
        check(connect_decoder_feed(&p, wire, split, frame, &actual) == 0, "Connect prefix");
        check(connect_decoder_feed(&p, wire + split, sizeof wire - split, frame, &actual) == 0,
              "Connect suffix");
        check(actual.hash == expected.hash && actual.count == expected.count &&
                  connect_decoder_pending(&p),
              "Connect split bytes");
        connect_decoder_free(&p);
    }
}
void limits() {
    constexpr uint32_t limit = 64u * 1024u * 1024u;
    std::array<char, 16384> block{};
    for (uint32_t length : {limit - 1, limit, limit + 1}) {
        char header[] = {0, char(length >> 24), char(length >> 16), char(length >> 8),
                         char(length)};
        connect_decoder p{};
        capture out;
        int status = connect_decoder_feed(&p, header, sizeof header, frame, &out);
        check(status == (length > limit ? -1 : 0), "64 MiB frame-size check");
        if (!status) {
            for (size_t left = length; left;) {
                size_t count = std::min(left, block.size());
                check(connect_decoder_feed(&p, block.data(), count, frame, &out) == 0,
                      "legal frame payload");
                left -= count;
            }
            check(out.count == 1 && !connect_decoder_pending(&p), "exact legal frame delivered");
        }
        connect_decoder_free(&p);
    }
}
void lifetime_and_identity() {
    decoding d;
    auto add = [&](const char *json) {
        check(oa_decode_event(true, json, std::strlen(json), &d.calls, decoded, &d.out) == 0,
              "tool event decode");
    };
    add(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"one","function":{"name":"terminal","arguments":"a"}}]}}]})");
    add(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"two","function":{"name":"read_file","arguments":"b"}}]}}]})");
    add(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"c"}}]}}]})");
    check(d.calls.n == 2, "fresh ID must take precedence over repeated index");
    check(std::strcmp(d.calls.calls[0].id, "one") == 0 &&
              std::strcmp(d.calls.calls[0].args.data, "a") == 0,
          "document lifetime owned ID/arguments");
    check(std::strcmp(d.calls.calls[1].id, "two") == 0 &&
              std::strcmp(d.calls.calls[1].args.data, "bc") == 0,
          "latest index continuation");
    for (int i = 2; i <= OA_MAX_TOOL_CALLS; i++) {
        char json[256];
        std::snprintf(
            json, sizeof json,
            R"({"choices":[{"delta":{"tool_calls":[{"id":"id%d","function":{"arguments":"x"}}]}}]})",
            i);
        add(json);
        check(d.calls.n == std::min(i + 1, OA_MAX_TOOL_CALLS), "32-call boundary");
    }
    check(oa_decode_event(true, "{", 1, &d.calls, decoded, &d.out) == 1,
          "malformed JSON distinguished");
}
void faults() {
    for (unsigned mode : {0u, 1u, 2u}) {
        const char connect[] = {0,   0,   0,   0,   32,  'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
                                'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u',
                                'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5'};
        std::string_view wire = mode == 0 ? std::string_view(connect, sizeof connect)
                                          : std::string_view(mode == 1 ? chat : responses);
        tny_alloc_scope_begin("parser-smoke");
        auto baseline = consume(wire, 1, mode);
        size_t count = tny_alloc_test_scope_count();
        check(count > 0 && baseline.status == 0, "fault baseline allocations");
        for (size_t index = 1; index <= count; index++) {
            char number[32];
            std::snprintf(number, sizeof number, "%zu", index);
            check(setenv("TNY_TEST_ALLOC_SCOPE", "parser-smoke", 1) == 0, "set scope");
            check(setenv("TNY_TEST_ALLOC_FAIL_AT", number, 1) == 0, "set index");
            for (int repeat = 0; repeat < 2; repeat++) {
                tny_alloc_scope_begin("parser-smoke");
                auto failed = consume(wire, 1, mode);
                check(tny_alloc_test_scope_injected(), "fault index was exercised");
                check(failed.status == -2,
                      "OOM must not be swallowed as success or malformed JSON");
            }
            unsetenv("TNY_TEST_ALLOC_SCOPE");
            unsetenv("TNY_TEST_ALLOC_FAIL_AT");
            tny_alloc_scope_begin("parser-smoke");
            check(consume(wire, 1, mode) == baseline, "successful later decode after two OOMs");
        }
        std::printf("parser mode %u: %zu allocation indices, two failures then recovery each\n",
                    mode, count);
    }
}
} // namespace
int main(int argc, char **argv) {
    tny_alloc_scope_begin("parser-smoke");
    split_positions();
    lifetime_and_identity();
    faults();
    limits();
    for (auto wire : {"", chat, responses, "data: {\n\n", "data:\r\n\r\n"}) {
        for (unsigned char mode : {0, 1, 2}) {
            std::string input(1, static_cast<char>(mode));
            input += wire;
            LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t *>(input.data()), input.size());
        }
    }
    for (int i = 1; i < argc; i++) {
        FILE *file = std::fopen(argv[i], "rb");
        check(file != nullptr, "open corpus seed");
        std::array<uint8_t, 131072> bytes{};
        size_t size = std::fread(bytes.data(), 1, bytes.size(), file);
        check(!std::ferror(file), "read corpus seed");
        std::fclose(file);
        LLVMFuzzerTestOneInput(bytes.data(), size);
    }
    std::puts("parser smoke passed: splits, lifetime, identity, limits, OOM and corpus");
}
#endif
