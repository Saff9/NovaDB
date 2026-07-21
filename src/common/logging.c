/*
 * logging.c — Structured, level-gated logging
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <pthread.h>
#include "logging.h"

static LogLevel          g_threshold = LOG_NOTICE;
static pthread_mutex_t   g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static int               g_initialized = 0;

static void init_once(void) {
    if (g_initialized) return;
    const char *env = getenv("NVDB_LOG");
    if (env) {
        if (!strcasecmp(env, "debug"))  g_threshold = LOG_DEBUG;
        else if (!strcasecmp(env, "info"))   g_threshold = LOG_INFO;
        else if (!strcasecmp(env, "warn"))   g_threshold = LOG_WARN;
        else if (!strcasecmp(env, "error"))  g_threshold = LOG_ERROR;
    }
    g_initialized = 1;
}

static const char *label_for(LogLevel lv) {
    switch (lv) {
    case LOG_DEBUG:  return "DEBUG";
    case LOG_INFO:   return "INFO";
    case LOG_NOTICE: return "NOTICE";
    case LOG_WARN:   return "WARN";
    case LOG_ERROR:  return "ERROR";
    default:         return "?????";
    }
}

void nvdb_log_impl(LogLevel lv, const char *func,
                   const char *fmt, ...) {
    init_once();
    if (lv < g_threshold) return;

    va_list ap;
    va_start(ap, fmt);

    pthread_mutex_lock(&g_log_lock);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    fprintf(stderr, "%04d-%02d-%02dT%02d:%02d:%02d ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
    fprintf(stderr, "[%-6s] %-24s ", label_for(lv), func);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);

    pthread_mutex_unlock(&g_log_lock);
    va_end(ap);
}
