#ifndef BEIGE_COMPAT_H
#define BEIGE_COMPAT_H

/* ISO C99 and POSIX.1-2001. Fallbacks cover hosts whose libc predates those headers. */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if !defined(BEIGE_NO_C99_HEADERS)
#include <stdint.h>
#include <stdbool.h>
#else
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;
typedef int bool;
#define true 1
#define false 0
#endif

#endif
