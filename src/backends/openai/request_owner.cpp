#include "backends/openai/request_owner.h"
#include "json/ownership.hpp"
extern "C" {
#include "core/provider_extras.h"
}
#include <array>
#include <cstring>
#include <limits>

namespace tny_native_request_detail {
struct connection_deleter {
    void operator()(http_conn *p) const noexcept { http_close(p); }
};
/* Allocate once, before writing any secret. No grow/realloc operation can
 * release an unwiped copy, including an exception during later setup. */
struct secret_header {
    tny::c_string bytes;
    size_t size = 0;
    secret_header() = default;
    secret_header(const secret_header &) = delete;
    secret_header &operator=(const secret_header &) = delete;
    ~secret_header() noexcept {
        if (bytes) secure_zero(bytes.get(), size);
    }
    void build(const char *name, const char *prefix, const char *key) {
        const char *parts[] = {name, ": ", prefix, key};
        size_t total = 1;
        for (const char *part : parts) {
            size_t len = std::strlen(part);
            if (len > std::numeric_limits<size_t>::max() - total) throw std::bad_alloc();
            total += len;
        }
        bytes.reset(tny::required(static_cast<char *>(tny_alloc_malloc(total))));
        size = total;
        char *out = bytes.get();
        for (const char *part : parts) {
            size_t len = std::strlen(part);
            std::memcpy(out, part, len);
            out += len;
        }
        *out = '\0';
    }
};
} // namespace tny_native_request_detail

struct oa_connection_owner {
    std::unique_ptr<http_conn, tny_native_request_detail::connection_deleter> connection;
};

namespace tny_native_request_detail {
struct build_buffer {
    buf_t value{};
    build_buffer() = default;
    build_buffer(const build_buffer &) = delete;
    build_buffer &operator=(const build_buffer &) = delete;
    ~build_buffer() noexcept { buf_free(&value); }
};
} // namespace tny_native_request_detail
struct oa_request_owner {
    std::array<tny_native_request_detail::build_buffer, OA_BUILD_BUFFER_COUNT> scratch;
    std::array<tny::c_string, OA_BUILD_STRING_COUNT> strings;
    tny::mutable_document view;
    tny::c_string body;
    tny_native_request_detail::secret_header auth;
    tny::string path;
    std::array<tny::c_string, 4> addons;
    /* At most 11 standard/profile + 4 add-ons + 3 affinity + terminator. */
    static_assert(11 + 4 + 3 + 1 <= 20);
    std::array<const char *, 20> headers{};
    size_t body_len = 0;
    bool attempted = false;
    bool prepared = false;

    void prepare(const oa_request_options &options) {
        if (!body || tny_alloc_scope_failed()) throw std::bad_alloc();
        body_len = std::strlen(body.get());
        size_t hn = 0;
        headers[hn++] = "Content-Type: application/json";
        headers[hn++] = "Accept: text/event-stream";
        if (options.api_key) {
            auth.build(options.auth_name ? options.auth_name : "Authorization",
                       options.auth_prefix ? options.auth_prefix : "Bearer ", options.api_key);
            headers[hn++] = auth.bytes.get();
        }
        for (char *const *e = options.extra_headers; e && *e && hn < 11; ++e) headers[hn++] = *e;
        tny_request_scope scope{options.provider_name, options.base_url, options.session_id};
        char *extra[4]{};
        int count = tny_provider_extras_headers(&scope, extra, 4);
        /* Adopt every returned allocation before checking the allocator latch. */
        for (int i = 0; i < count; ++i) addons[static_cast<size_t>(i)].reset(extra[i]);
        if (tny_alloc_scope_failed()) throw std::bad_alloc();
        for (int i = 0; i < count; ++i) headers[hn++] = addons[static_cast<size_t>(i)].get();
        if (options.session_header) headers[hn++] = options.session_header;
        if (options.thread_header) headers[hn++] = options.thread_header;
        if (options.state_header) headers[hn++] = options.state_header;
        headers[hn] = nullptr;
        path.reserve(std::strlen(options.prefix) + std::strlen(options.endpoint));
        path = options.prefix;
        path += options.endpoint;
        prepared = true;
    }
};

extern "C" oa_request_owner *oa_request_new(void) {
    try {
        return tny::make_owned<oa_request_owner>().release();
    } catch (...) { return nullptr; }
}
extern "C" buf_t *oa_request_buffer(oa_request_owner *request, oa_build_buffer which) {
    return &request->scratch[which].value;
}
extern "C" const char *oa_request_take_string(oa_request_owner *request, oa_build_string which,
                                              char *owned) {
    request->strings[which].reset(owned);
    return request->strings[which].get();
}
extern "C" yyjson_mut_doc *oa_request_take_view(oa_request_owner *request, yyjson_mut_doc *view) {
    request->view.reset(view);
    return request->view.get();
}
#ifdef TNY_ALLOC_TESTING
static thread_local bool builder_released_view;
extern "C" bool oa_request_test_builder_released_view(void) { return builder_released_view; }
#endif
extern "C" int oa_request_prepare(oa_request_owner *request, char *body,
                                  const oa_request_options *options) {
    tny::c_string incoming(body);
    if (request->attempted) return -2;
    request->attempted = true;
#ifdef TNY_ALLOC_TESTING
    builder_released_view = !request->view;
#endif
    request->view.reset();
    request->body = std::move(incoming);
    try {
        request->prepare(*options);
        return 0;
    } catch (...) { return -2; }
}
extern "C" void oa_request_free(oa_request_owner **request) {
    tny::owned<oa_request_owner> released(*request);
    *request = nullptr;
}
extern "C" oa_connection_owner *oa_connection_new(void) {
    try {
        return tny::make_owned<oa_connection_owner>().release();
    } catch (...) { return nullptr; }
}
extern "C" int oa_connection_open(oa_connection_owner *owner, const char *url, char *err,
                                  size_t errlen) {
    owner->connection.reset();
    owner->connection.reset(http_open(url, err, errlen));
    if (tny_alloc_scope_failed()) {
        owner->connection.reset();
        return -2;
    }
    return owner->connection ? 0 : -1;
}
extern "C" http_conn *oa_connection_get(const oa_connection_owner *owner) {
    return owner->connection.get();
}
extern "C" void oa_connection_drop(oa_connection_owner *owner) { owner->connection.reset(); }
extern "C" void oa_connection_free(oa_connection_owner **owner) {
    tny::owned<oa_connection_owner> released(*owner);
    *owner = nullptr;
}
extern "C" int oa_request_send(oa_request_owner *request, oa_connection_owner *connection) {
    http_conn *conn = oa_connection_get(connection);
    if (!conn || !request->prepared) return -2;
    int rc = http_request(conn, "POST", request->path.c_str(), request->headers.data(),
                          request->body.get(), request->body_len);
    return tny_alloc_scope_failed() ? -2 : rc;
}
