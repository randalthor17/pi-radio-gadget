#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); g_failures++; } } while (0)

#define TEST_DONE(name) do { if (g_failures) { fprintf(stderr, "%s: %d failure(s)\n", name, g_failures); return 1; } \
    printf("%s: all checks passed\n", name); return 0; } while (0)
