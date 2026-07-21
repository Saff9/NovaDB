/*
 * executor.h — SQL executor interface
 */
#ifndef NOVDB_EXECUTOR_H
#define NOVDB_EXECUTOR_H

#include "novadb/types.h"
#include "parser.h"

/* ── Result set ───────────────────────────────────────────────── */
typedef struct {
    int         ncols;
    char      **col_names;
    int         nrows;
    NVDBValue  **rows;     /* rows[nrows][ncols]                  */
    char       *message;   /* for DDL / DML status messages       */
    char       *error;     /* on failure                          */
} ExecResult;

/* ── API ──────────────────────────────────────────────────────── */
ExecResult *executor_run(NVDBEngine *engine, ASTStmt *stmt);
void        executor_free_result(ExecResult *res);

#endif /* NOVDB_EXECUTOR_H */
