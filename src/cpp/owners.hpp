/* Private ownership only. C callbacks must not throw or retain borrowed views. */
#ifndef TNY_CPP_OWNERS_HPP
#define TNY_CPP_OWNERS_HPP

#include "cpp/testing.h"
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
extern "C" {
#include "util/alloc.h"
#include "json/json.h"
}

namespace tny {
inline void allocated() noexcept {
#ifdef TNY_ALLOC_TESTING
    tny_parser_test_allocated();
#endif
}
inline void freed() noexcept {
#ifdef TNY_ALLOC_TESTING
    tny_parser_test_freed();
#endif
}
template <class T> struct allocator {
    using value_type = T;
    allocator() noexcept = default;
    template <class U> allocator(const allocator<U> &) noexcept {}
    [[nodiscard]] T *allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_alloc();
        auto *p = static_cast<T *>(tny_alloc_malloc(count * sizeof(T)));
        if (!p) throw std::bad_alloc();
        allocated();
        return p;
    }
    void deallocate(T *p, std::size_t) noexcept {
        std::free(p);
        freed();
    }
    template <class U> bool operator==(const allocator<U> &) const noexcept { return true; }
};
using string = std::basic_string<char, std::char_traits<char>, allocator<char>>;

/* Objects use the same boundary as their member containers. */
template <class T> struct destroy {
    void operator()(T *p) const noexcept {
        if (p) {
            p->~T();
            std::free(p);
            freed();
        }
    }
};
template <class T> using owner = std::unique_ptr<T, destroy<T>>;
template <class T> owner<T> make_owner() {
    void *p = tny_alloc_malloc(sizeof(T));
    if (!p) throw std::bad_alloc();
    allocated();
    try {
        return owner<T>(::new (p) T());
    } catch (...) {
        std::free(p);
        freed();
        throw;
    }
}
struct doc_delete {
    void operator()(yyjson_doc *p) const noexcept { yyjson_doc_free(p); }
};
using document = std::unique_ptr<yyjson_doc, doc_delete>;
static_assert(!std::is_copy_constructible_v<document>);
static_assert(std::is_nothrow_destructible_v<document>);

/* Invalid JSON is an empty owner; allocator exhaustion is never a parse error. */
inline document parse(std::string_view bytes) {
    yyjson_read_err err{};
    document doc(
        yyjson_read_opts(const_cast<char *>(bytes.data()), bytes.size(), 0, jallocator(), &err));
    if (!doc && err.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION) throw std::bad_alloc();
    return doc;
}

/* Reserve checked arithmetic before asking basic_string to grow. */
inline void append(string &to, std::string_view bytes) {
    if (bytes.size() > to.max_size() - to.size()) throw std::bad_alloc();
    if (!bytes.empty()) to.append(bytes.data(), bytes.size());
}
} // namespace tny
#endif
