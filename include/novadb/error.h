/*
 * error.h — Error handling and diagnostic utilities
 *
 * NovaDB uses return-code-based error handling throughout.
 * Functions that can fail return nvdb_errcode_t; the caller
 * must check the return and propagate or handle errors.
 *
 * Error context (file, line, function) is captured via macros
 * so the call site reads cleanly while preserving diagnostics.
 */
#ifndef NOVADB_ERROR_H
#define NOVADB_ERROR_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Error context ───────────────────────────────────────────── */
typedef struct {
    nvdb_errcode_t code;
    const char    *file;
    int            line;
    const char    *func;
    char           message[256];
} NVDBError;

/* ── Thread-local last error ─────────────────────────────────── */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  include <threads.h>
#  define NVDB_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#  define NVDB_THREAD_LOCAL __thread
#else
#  define NVDB_THREAD_LOCAL
#endif

extern NVDB_THREAD_LOCAL NVDBError nvdb_last_error;

/* ── Error setting macros ────────────────────────────────────── */
#define nvdb_set_error(code_, fmt_, ...)                               \
    do {                                                               \
        nvdb_last_error.code = (code_);                                \
        nvdb_last_error.file = __FILE__;                               \
        nvdb_last_error.line = __LINE__;                               \
        nvdb_last_error.func = __func__;                               \
        snprintf(nvdb_last_error.message,                              \
                 sizeof(nvdb_last_error.message),                      \
                 (fmt_), ##__VA_ARGS__);                               \
    } while (0)

/* ── Return-on-error helper ───────────────────────────────────── */
#define nvdb_try(expr_)                                                \
    do {                                                               \
        nvdb_errcode_t _rc = (expr_);                                  \
        if (NVDB_UNLIKELY(_rc != NVDB_OK)) return _rc;                 \
    } while (0)

/* ── Assertions ───────────────────────────────────────────────── */
#if !defined(NDEBUG)
#  define nvdb_assert(cond_)                                          \
       do {                                                           \
           if (!(cond_)) {                                            \
               nvdb_panic(__FILE__, __LINE__, __func__,               \
                          "assertion failed: " #cond_);               \
           }                                                         \
       } while (0)
#else
#  define nvdb_assert(cond_) ((void)0)
#endif

/* ── Panic (unrecoverable) ────────────────────────────────────── */
NVDB_NORETURN void nvdb_panic(const char *file, int line,
                               const char *func, const char *msg);

/* ── Error string conversion ──────────────────────────────────── */
const char *nvdb_strerror(nvdb_errcode_t code);

/* ── Clear last error ─────────────────────────────────────────── */
static inline void nvdb_clear_error(void) {
    nvdb_last_error.code = NVDB_OK;
    nvdb_last_error.message[0] = '\0';
}

#ifdef __cplusplus
}
#endif

#endif /* NOVADB_ERROR_H */
