/* Private resource owners. Lifecycle proof and persistence remain explicit. */
#ifndef TNY_UTIL_RESOURCES_HPP
#define TNY_UTIL_RESOURCES_HPP
#include <cerrno>
#include <type_traits>
#include <utility>
#include <unistd.h>
extern "C" {
#include "core/session.h"
#include "util/process.h"
}
namespace tny {
struct descriptor_tag {};
struct lock_tag {};
/* Close only: LOCK_UN would unlock a fork-inherited open-file description. */
template <class Tag> class basic_descriptor {
    int value_ = -1;

  public:
    basic_descriptor() noexcept = default;
    ~basic_descriptor() noexcept { reset(); }
    basic_descriptor(const basic_descriptor &) = delete;
    basic_descriptor &operator=(const basic_descriptor &) = delete;
    basic_descriptor(basic_descriptor &&other) noexcept : value_(other.release()) {}
    basic_descriptor &operator=(basic_descriptor &&other) noexcept {
        if (this != &other) adopt(other.release());
        return *this;
    }
    [[nodiscard]] int borrow() const noexcept { return value_; }
    [[nodiscard]] int release() noexcept { return std::exchange(value_, -1); }
    void reset() noexcept {
        int saved = errno;
        if (value_ >= 0) close(release());
        errno = saved;
    }
    void adopt(int value) noexcept {
        reset();
        value_ = value;
    }
};
using descriptor = basic_descriptor<descriptor_tag>;
using lock_descriptor = basic_descriptor<lock_tag>;
struct pipe_pair {
    descriptor ends[2];
    pipe_pair() noexcept = default;
    pipe_pair(const pipe_pair &) = delete;
    pipe_pair &operator=(const pipe_pair &) = delete;
    pipe_pair(pipe_pair &&) noexcept = default;
    pipe_pair &operator=(pipe_pair &&) noexcept = default;
    ~pipe_pair() noexcept {
        // Name both owned ends explicitly, including on exception unwind.
        // The member destructors then observe empty, idempotent owners.
        ends[0].reset();
        ends[1].reset();
    }
    int open() noexcept {
        int raw[2];
        if (pipe(raw) != 0) return -1;
        ends[0].adopt(raw[0]);
        ends[1].adopt(raw[1]);
        return 0;
    }
};
/* No destructor can infer cleanup success. Unretired authority transfers to
 * the C supervisor-lifetime list, never to a dangling local or a PID record. */
class process_scope {
    tny_process_scope *value_ = nullptr;

  public:
    process_scope() noexcept = default;
    ~process_scope() noexcept {
        if (value_) tny_process_scope_retain_until_exit(value_);
    }
    process_scope(const process_scope &) = delete;
    process_scope &operator=(const process_scope &) = delete;
    process_scope(process_scope &&other) noexcept : value_(other.release()) {}
    process_scope &operator=(process_scope &&other) noexcept {
        if (this != &other) {
            if (value_) tny_process_scope_retain_until_exit(value_);
            value_ = other.release();
        }
        return *this;
    }
    [[nodiscard]] tny_process_scope *borrow() const noexcept { return value_; }
    [[nodiscard]] tny_process_scope *release() noexcept { return std::exchange(value_, nullptr); }
    void adopt(tny_process_scope *value) noexcept {
        if (value_) tny_process_scope_retain_until_exit(value_);
        value_ = value;
    }
    [[nodiscard]] int retire() noexcept {
        int rc = tny_process_scope_destroy(value_);
        if (!rc) value_ = nullptr;
        return rc;
    }
};
/* Owns the session's existing lock slot, never a second raw descriptor. An
 * already-locked caller is borrowed and remains responsible for its copy. */
class spawn_writer {
    tny_session_state *session_;
    bool owned_;

  public:
    spawn_writer(tny_session_state *session, bool owned) noexcept
        : session_(session), owned_(owned) {}
    ~spawn_writer() noexcept {
        if (owned_) session_lock_release(session_);
    }
    void release() noexcept { owned_ = false; }
    spawn_writer(const spawn_writer &) = delete;
    spawn_writer &operator=(const spawn_writer &) = delete;
};
static_assert(!std::is_copy_constructible_v<descriptor>);
static_assert(std::is_nothrow_move_constructible_v<descriptor>);
static_assert(std::is_nothrow_destructible_v<lock_descriptor>);
} // namespace tny
#endif
