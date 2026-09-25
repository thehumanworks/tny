#ifndef TNY_CXX_GLIBC_FLOOR_H
#define TNY_CXX_GLIBC_FLOOR_H

/* libstdc++ and the native C spawn flags define _GNU_SOURCE. glibc 2.38+
 * then redirects strtol and friends to __isoc23_*@GLIBC_2.38. Force-include
 * this before C/C++ standard or allocator headers to keep the published
 * 2.34 floor. The historical filename is retained for build consumers. */
#if defined(__linux__) && !defined(__ANDROID__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <features.h>
#ifdef __GLIBC__
#undef __GLIBC_USE_ISOC23
#define __GLIBC_USE_ISOC23 0
#undef __GLIBC_USE_ISOC2X
#define __GLIBC_USE_ISOC2X 0
/* features.h computes these derived switches before the overrides above.
 * Reset them too; otherwise stdlib/stdio still import GLIBC_2.38 symbols. */
#ifdef __GLIBC_USE_C2X_STRTOL
#undef __GLIBC_USE_C2X_STRTOL
#define __GLIBC_USE_C2X_STRTOL 0
#endif
#ifdef __GLIBC_USE_C23_STRTOL
#undef __GLIBC_USE_C23_STRTOL
#define __GLIBC_USE_C23_STRTOL 0
#endif
#endif
#endif

#endif
