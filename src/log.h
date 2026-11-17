/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal thread-safe logging. One fprintf per line so threads don't interleave. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { LOG_ERR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DBG = 3, LOG_TRACE = 4 };

void log_set_level(int level);
/* Also append every line (at most max_bytes in total, then it stops) to a file, truncated now. For a file on the SD card's
 * boot partition that a PC can read when the Pi is unreachable. */
void log_set_file(const char *path, size_t max_bytes);
int  log_level(void);
void logmsg(int level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOGE(tag, ...) logmsg(LOG_ERR,  tag, __VA_ARGS__)
#define LOGW(tag, ...) logmsg(LOG_WARN, tag, __VA_ARGS__)
#define LOGI(tag, ...) logmsg(LOG_INFO, tag, __VA_ARGS__)
#define LOGD(tag, ...) logmsg(LOG_DBG,  tag, __VA_ARGS__)
#define LOGT(tag, ...) logmsg(LOG_TRACE, tag, __VA_ARGS__)

/* Monotonic milliseconds. */
uint64_t now_ms(void);
void     sleep_ms(unsigned ms);

/* Hex of up to `max` leading bytes into out (always NUL terminated). */
void hex_prefix(const void *data, size_t len, size_t max, char *out, size_t out_cap);

/* Logs at error level and terminates the whole process (systemd restarts the service,
 * and the setup script tears the gadget down so the next start is clean). */
void log_fatal(const char *tag, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 2, 3)));
