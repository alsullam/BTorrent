#define _POSIX_C_SOURCE 200809L
/**
 * log.c — Logging implementation
 */

#include "log.h"
#include <stdarg.h>
#include <string.h>
#include <time.h>

static LogLevel g_min_level = LOG_INFO;
static FILE    *g_dest      = NULL;

static const char *const level_tags[] = {
    "DEBUG", "INFO ", "WARN ", "ERROR"
};

void log_init(LogLevel min_level, FILE *dest) {
    g_min_level = min_level;
    g_dest      = dest ? dest : stderr;
}

void _log_write(LogLevel level, const char *file, int line,
                const char *fmt, ...) {
    if (level < g_min_level) return;

    FILE *out = g_dest ? g_dest : stderr;

    /* HH:MM:SS timestamp */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[10];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);

    /* Basename only — strip directory prefix */
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;

    fprintf(out, "\r\033[K[%s] %s  %s:%d  ", ts, level_tags[level], base, line);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);

    fputc('\n', out);

    /* Flush immediately for WARN/ERROR so messages survive crashes */
    if (level >= LOG_WARN) fflush(out);
}
