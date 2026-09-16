/* Private allocation accounting, present only in fault/test objects. */
#ifndef TNY_CPP_TESTING_H
#define TNY_CPP_TESTING_H
#include <stddef.h>
#ifdef TNY_ALLOC_TESTING
#ifdef __cplusplus
extern "C" {
#endif
size_t tny_parser_test_live_allocations(void);
void tny_parser_test_allocated(void);
void tny_parser_test_freed(void);
#ifdef __cplusplus
}
#endif
#endif
#endif
