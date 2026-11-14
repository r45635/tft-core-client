// A test harness small enough to read in one sitting.
#pragma once

#include <cstdio>
#include <cstring>

namespace check {
inline int checks = 0;
inline int failures = 0;
inline const char *suite = "";

inline void fail(const char *file, int line, const char *what) {
    failures++;
    printf("  FAIL %s:%d  %s\n", file, line, what);
}
}  // namespace check

#define CHECK(cond)                                      \
    do {                                                 \
        check::checks++;                                 \
        if (!(cond)) check::fail(__FILE__, __LINE__, #cond); \
    } while (0)

#define CHECK_STR(got, want)                                                       \
    do {                                                                           \
        check::checks++;                                                           \
        if (strcmp((got), (want)) != 0) {                                          \
            char msg[256];                                                         \
            snprintf(msg, sizeof msg, "got \"%s\", want \"%s\"", (got), (want));   \
            check::fail(__FILE__, __LINE__, msg);                                  \
        }                                                                          \
    } while (0)

#define CHECK_INT(got, want)                                                        \
    do {                                                                            \
        check::checks++;                                                            \
        long long g = (long long)(got), w = (long long)(want);                      \
        if (g != w) {                                                               \
            char msg[160];                                                          \
            snprintf(msg, sizeof msg, #got " = %lld, want %lld", g, w);             \
            check::fail(__FILE__, __LINE__, msg);                                   \
        }                                                                           \
    } while (0)

void testMoo();
void testSood();
void testWs();
void testZones();
