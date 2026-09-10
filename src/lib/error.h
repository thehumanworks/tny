/* Shared private error construction for public ABI adapters. */
#ifndef TNY_LIB_ERROR_H
#define TNY_LIB_ERROR_H
#include "tny/tny.h"
int32_t tny_lib_error(tny_error **out, int32_t status, const char *message);
#endif
