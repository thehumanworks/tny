#ifndef TNY_CXX_GLIBC_FLOOR_H
#define TNY_CXX_GLIBC_FLOOR_H

/* libstdc++ defines _GNU_SOURCE, and glibc 2.38+ then redirects strtol
 * and friends to __isoc23_*@GLIBC_2.38. Force-include this before any C++
 * standard header so private C++20 TUs keep the published 2.34 floor. */
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
#endif
#endif

#endif
