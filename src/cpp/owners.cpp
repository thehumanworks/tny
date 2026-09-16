#include "cpp/testing.h"
#ifdef TNY_ALLOC_TESTING
#include <atomic>
namespace {
std::atomic<size_t> live_allocations{0};
}
extern "C" __attribute__((visibility("default"))) size_t tny_parser_test_live_allocations(void) {
    return live_allocations.load(std::memory_order_relaxed);
}
extern "C" void tny_parser_test_allocated(void) {
    live_allocations.fetch_add(1, std::memory_order_relaxed);
}
extern "C" void tny_parser_test_freed(void) {
    live_allocations.fetch_sub(1, std::memory_order_relaxed);
}
#endif
