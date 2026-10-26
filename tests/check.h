#ifndef BEIGE_CHECK_H
#define BEIGE_CHECK_H

#include <stdio.h>

static int g_checks_failed = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            g_checks_failed++;                                                      \
        }                                                                           \
    } while (0)

#endif
