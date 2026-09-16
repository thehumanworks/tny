/* Private, fault-injected ownership. No global operator new replacement. */
#ifndef TNY_OWNERSHIP_HPP
#define TNY_OWNERSHIP_HPP

#include "util/alloc.h"
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace tny {
template <class T> struct allocator {
    using value_type = T;
    using is_always_equal = std::true_type;
    allocator() noexcept = default;
    template <class U> allocator(const allocator<U> &) noexcept {}
    [[nodiscard]] T *allocate(std::size_t n) {
        static_assert(alignof(T) <= alignof(std::max_align_t));
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_alloc();
        auto *p = static_cast<T *>(tny_alloc_malloc(n * sizeof(T)));
        if (!p) throw std::bad_alloc();
#ifdef TNY_ALLOC_TESTING
        tny_alloc_test_owned_acquire();
#endif
        return p;
    }
    void deallocate(T *p, std::size_t) noexcept {
        std::free(p);
#ifdef TNY_ALLOC_TESTING
        if (p) tny_alloc_test_owned_release();
#endif
    }
    template <class U> bool operator==(const allocator<U> &) const noexcept { return true; }
};
struct free_deleter {
    void operator()(void *p) const noexcept { std::free(p); }
};
template <class T> struct object_deleter {
    void operator()(T *p) const noexcept {
        static_assert(std::is_nothrow_destructible_v<T>);
        if (p) {
            p->~T();
            std::free(p);
#ifdef TNY_ALLOC_TESTING
            tny_alloc_test_owned_release();
#endif
        }
    }
};
template <class T> using owned = std::unique_ptr<T, object_deleter<T>>;
template <class T, class... Args> owned<T> make_owned(Args &&...args) {
    static_assert(alignof(T) <= alignof(std::max_align_t));
    std::unique_ptr<void, free_deleter> storage(tny_alloc_malloc(sizeof(T)));
    if (!storage) throw std::bad_alloc();
    ::new (storage.get()) T(std::forward<Args>(args)...);
#ifdef TNY_ALLOC_TESTING
    tny_alloc_test_owned_acquire();
#endif
    return owned<T>(static_cast<T *>(storage.release()));
}
using c_string = std::unique_ptr<char, free_deleter>;
using string = std::basic_string<char, std::char_traits<char>, allocator<char>>;
template <class T> using vector = std::vector<T, allocator<T>>;
using bytes = vector<unsigned char>;
/* Owners containing these values delete copy operations. Views are rebuilt
 * after mutation/move; in particular never retain a small-string pointer. */
} // namespace tny
#endif
