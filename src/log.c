/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "log.h"

#include <pthread.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_level = LOG_INFO;
static FILE *g_file;
static size_t g_file_max, g_file_len;
static pthread_mutex_t g_file_mu = PTHREAD_MUTEX_INITIALIZER;
static const char *const names[] = { "ERR", "WRN", "INF", "DBG", "TRC" };

void log_set_level(int level) { g_level = level; }

void log_set_file(const char *path, size_t max_bytes)
{
    /* Append across restarts (a crash loop must not wipe the evidence); start over only when the file is full. */
    struct stat st;
    size_t have = stat(path, &st) == 0 ? (size_t)st.st_size : 0;
    g_file = fopen(path, have >= max_bytes ? "w" : "a");
    g_file_max = max_bytes;
    g_file_len = have >= max_bytes ? 0 : have;
    if (g_file)
        fprintf(g_file, "---- start ----\n");
}
int  log_level(void) { return g_level; }

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0)
        ;                       /* resumes with the remaining time after EINTR */
}

void logmsg(int level, const char *tag, const char *fmt, ...)
{
    if (level > g_level)
        return;
    char msg[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    /* Wall-clock time (HH:MM:SS.mmm) so logs can be lined up with the PC's kernel log. */
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    char line[760];
    int ln = snprintf(line, sizeof line, "%02d:%02d:%02d.%03ld %s %-5s %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
                      ts.tv_nsec / 1000000L, names[level > LOG_TRACE ? LOG_TRACE : level], tag, msg);
    fputs(line, stderr);
    if (g_file && level <= LOG_INFO && ln > 0) {          /* info and above only: the card is not for debug spam */
        pthread_mutex_lock(&g_file_mu);
        if (g_file_len + (size_t)ln <= g_file_max) {
            fputs(line, g_file);
            fflush(g_file);
            g_file_len += (size_t)ln;
        }
        pthread_mutex_unlock(&g_file_mu);
    }
}

void hex_prefix(const void *data, size_t len, size_t max, char *out, size_t out_cap)
{
    const unsigned char *p = data;
    size_t n = len < max ? len : max, o = 0;
    for (size_t i = 0; i < n && o + 3 < out_cap; i++)
        o += (size_t)snprintf(out + o, out_cap - o, "%02x", p[i]);
    if (n < len && o + 4 < out_cap)
        o += (size_t)snprintf(out + o, out_cap - o, "...");
    if (out_cap)
        out[o < out_cap ? o : out_cap - 1] = '\0';
}

#include <stdlib.h>
#include <unistd.h>

void log_fatal(const char *tag, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    logmsg(LOG_ERR, tag, "FATAL: %s", msg);
    _exit(1);
}
