/*
 * logging.h — Internal logging interface
 */
#ifndef NOVDB_LOGGING_H
#define NOVDB_LOGGING_H

#include <stdarg.h>

typedef enum {
    LOG_DEBUG  = 0,
    LOG_INFO   = 1,
    LOG_NOTICE = 2,
    LOG_WARN   = 3,
    LOG_ERROR  = 4,
} LogLevel;

void nvdb_log_impl(LogLevel lv, const char *func,
                   const char *fmt, ...);

/* Convenience macros — the function name is captured automatically */
#define nvdb_log_debug(fmt, ...) \
    nvdb_log_impl(LOG_DEBUG, __func__, (fmt), ##__VA_ARGS__)

#define nvdb_log_info(fmt, ...) \
    nvdb_log_impl(LOG_INFO, __func__, (fmt), ##__VA_ARGS__)

#define nvdb_log_notice(fmt, ...) \
    nvdb_log_impl(LOG_NOTICE, __func__, (fmt), ##__VA_ARGS__)

#define nvdb_log_warn(fmt, ...) \
    nvdb_log_impl(LOG_WARN, __func__, (fmt), ##__VA_ARGS__)

#define nvdb_log_error(fmt, ...) \
    nvdb_log_impl(LOG_ERROR, __func__, (fmt), ##__VA_ARGS__)

#endif /* NOVDB_LOGGING_H */
