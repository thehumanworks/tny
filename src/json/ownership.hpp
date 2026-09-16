#ifndef TNY_JSON_OWNERSHIP_HPP
#define TNY_JSON_OWNERSHIP_HPP
#include "util/ownership.hpp"
#include "json/json.h"
namespace tny {
struct document_deleter {
    void operator()(yyjson_doc *p) const noexcept { yyjson_doc_free(p); }
};
struct mutable_document_deleter {
    void operator()(yyjson_mut_doc *p) const noexcept { yyjson_mut_doc_free(p); }
};
using document = std::unique_ptr<yyjson_doc, document_deleter>;
using mutable_document = std::unique_ptr<yyjson_mut_doc, mutable_document_deleter>;
/* Malformed input returns an empty owner; allocation failure is distinct. */
inline document parse(const char *data, size_t len) {
    yyjson_read_err error{};
    document doc(yyjson_read_opts(const_cast<char *>(data), len, 0, jallocator(), &error));
    if (!doc && error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION) throw std::bad_alloc();
    return doc;
}
inline mutable_document make_document() {
    mutable_document doc(yyjson_mut_doc_new(jallocator()));
    if (!doc) throw std::bad_alloc();
    return doc;
}
template <class T> T *required(T *p) {
    if (!p) throw std::bad_alloc();
    return p;
}
} // namespace tny
#endif
