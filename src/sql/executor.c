/*
 * executor.c — SQL statement executor
 *
 * Walks the AST and executes operations against the storage engine.
 * Row data is stored as key=value pair strings keyed by
 * "tablename:N" where N is an auto-incrementing row ID.
 *
 * Table catalog is persisted to "__cat__:tablename" keys.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <inttypes.h>
#include "executor.h"
#include "parser.h"
#include "lexer.h"
#include "memory.h"
#include "logging.h"
#include "novadb/error.h"
#include "novadb/nvdb.h"

/* ── Engine bridge (implemented in main.c) ────────────────────── */
extern int      engine_btree_insert(void *eng, const void *k, uint16_t kl,
                                     const void *v, uint16_t vl);
extern int      engine_btree_delete(void *eng, const void *k, uint16_t kl);
extern int      engine_btree_search(void *eng, const void *k, uint16_t kl,
                                     void *v, uint16_t *vl, uint16_t max);
extern int      engine_wal_log(void *eng, nvdb_walop_t op, txnid_t tid,
                                const void *k, uint16_t kl,
                                const void *v, uint32_t vl);
extern int      engine_wal_sync(void *eng);
extern int64_t  engine_next_rowid(void *eng, const char *table);

/* ── Catalog ──────────────────────────────────────────────────── */

typedef struct {
    char    *name;
    int      ncols;
    ASTColumnDef *cols;
} CatalogEntry;

static CatalogEntry g_catalog[256];
static int          g_ncatalog;

static CatalogEntry *cat_find(const char *name) {
    for (int i = 0; i < g_ncatalog; i++)
        if (strcasecmp(g_catalog[i].name, name) == 0)
            return &g_catalog[i];
    return NULL;
}

static int cat_add(const char *name, int ncols, ASTColumnDef *cols) {
    if (g_ncatalog >= 256) return -1;
    CatalogEntry *e = &g_catalog[g_ncatalog++];
    e->name  = nvdb_strdup(name);
    e->ncols = ncols;
    e->cols  = nvdb_malloc(sizeof(ASTColumnDef) * (size_t)ncols);
    memcpy(e->cols, cols, sizeof(ASTColumnDef) * (size_t)ncols);
    return 0;
}

static void cat_remove(const char *name) {
    for (int i = 0; i < g_ncatalog; i++) {
        if (strcasecmp(g_catalog[i].name, name) == 0) {
            free(g_catalog[i].name);
            free(g_catalog[i].cols);
            g_catalog[i] = g_catalog[--g_ncatalog];
            return;
        }
    }
}

/* ── Row encoding / decoding ──────────────────────────────────── */

static int encode_row(char *buf, size_t bufsz,
                      ASTColumnDef *cols, int ncols,
                      ASTExpr **values, int nvalues,
                      char **col_names, int ncol_names) {
    int pos = 0;
    for (int i = 0; i < ncols; i++) {
        const char *val = "NULL";
        char numbuf[64];

        if (col_names && ncol_names > 0) {
            for (int j = 0; j < ncol_names && j < nvalues; j++) {
                if (values[j] && values[j]->kind == AST_LITERAL
                    && strcasecmp(cols[i].name, col_names[j]) == 0) {
                    NVDBValue *v = &values[j]->lit;
                    switch (v->type) {
                    case NVDB_TYPE_INT64:
                        snprintf(numbuf, sizeof(numbuf), "%" PRId64, v->i64);
                        val = numbuf; break;
                    case NVDB_TYPE_FLOAT64:
                        snprintf(numbuf, sizeof(numbuf), "%g", v->f64);
                        val = numbuf; break;
                    case NVDB_TYPE_STRING:
                        val = v->str_val ? v->str_val : ""; break;
                    case NVDB_TYPE_BOOL:
                        val = v->bval ? "true" : "false"; break;
                    default: break;
                    }
                    break;
                }
            }
        }
        pos += snprintf(buf + pos, bufsz - (size_t)pos,
                        "%s=%s\n", cols[i].name, val);
    }
    return pos;
}

static void decode_row(const char *data, NVDBValue *out, int ncols,
                       ASTColumnDef *cols) {
    for (int i = 0; i < ncols; i++) {
        nvdb_val_set_null(&out[i]);

        const char *p = data;
        while (*p) {
            const char *eq = strchr(p, '=');
            const char *nl = strchr(p, '\n');
            if (!eq || !nl) break;

            size_t klen = (size_t)(eq - p);
            if (strncasecmp(p, cols[i].name, klen) == 0
                && strlen(cols[i].name) == klen) {
                const char *val = eq + 1;
                size_t vlen = (size_t)(nl - val);

                if (strncasecmp(cols[i].type, "INT", 3) == 0
                    || strncasecmp(cols[i].type, "INTEGER", 7) == 0
                    || strncasecmp(cols[i].type, "BIGINT", 6) == 0) {
                    nvdb_val_set_i64(&out[i], strtoll(val, NULL, 10));
                } else if (strncasecmp(cols[i].type, "FLOAT", 5) == 0
                           || strncasecmp(cols[i].type, "DOUBLE", 6) == 0) {
                    nvdb_val_set_f64(&out[i], strtod(val, NULL));
                } else if (strncasecmp(cols[i].type, "BOOL", 4) == 0
                           || strncasecmp(cols[i].type, "BOOLEAN", 7) == 0) {
                    nvdb_val_set_bool(&out[i],
                                       strncasecmp(val, "true", 4) == 0);
                } else {
                    char *s = nvdb_malloc(vlen + 1);
                    memcpy(s, val, vlen);
                    s[vlen] = '\0';
                    nvdb_val_set_str(&out[i], s, (uint32_t)vlen);
                }
                break;
            }
            p = nl + 1;
        }
    }
}

/* ── Expression evaluation ────────────────────────────────────── */

static bool eval_to_bool(ASTExpr *e, NVDBValue *row, int ncols,
                          ASTColumnDef *cols);
static NVDBValue eval_expr(ASTExpr *e, NVDBValue *row, int ncols,
                            ASTColumnDef *cols);

static NVDBValue eval_expr(ASTExpr *e, NVDBValue *row, int ncols,
                            ASTColumnDef *cols) {
    NVDBValue v;
    nvdb_val_set_null(&v);
    if (!e) return v;

    switch (e->kind) {
    case AST_LITERAL:
        return e->lit;

    case AST_COLUMN:
        for (int i = 0; i < ncols; i++) {
            if (strcasecmp(e->col.name, cols[i].name) == 0)
                return row[i];
        }
        break;

    case AST_BINARY: {
        NVDBValue l = eval_expr(e->bin.left, row, ncols, cols);
        NVDBValue r = eval_expr(e->bin.right, row, ncols, cols);

        if (l.type == NVDB_TYPE_INT64 && r.type == NVDB_TYPE_INT64) {
            nvdb_val_set_i64(&v, 0);
            switch (e->bin.op) {
            case AST_OP_ADD: v.i64 = l.i64 + r.i64; break;
            case AST_OP_SUB: v.i64 = l.i64 - r.i64; break;
            case AST_OP_MUL: v.i64 = l.i64 * r.i64; break;
            case AST_OP_DIV:
                v.i64 = (r.i64 != 0) ? l.i64 / r.i64 : 0; break;
            default:
                nvdb_val_set_bool(&v, eval_to_bool(e, row, ncols, cols));
                break;
            }
        } else {
            nvdb_val_set_bool(&v, eval_to_bool(e, row, ncols, cols));
        }
        break;
    }

    case AST_FUNCTION:
        if (strcasecmp(e->fn.name, "COUNT") == 0) {
            nvdb_val_set_i64(&v, 1);
        } else if (strcasecmp(e->fn.name, "UPPER") == 0 && e->fn.nargs > 0) {
            NVDBValue arg = eval_expr(e->fn.args[0], row, ncols, cols);
            const char *src = (arg.type == NVDB_TYPE_STRING)
                              ? (arg.str_val ? arg.str_val : "") : "";
            size_t slen = strlen(src);
            char *up = nvdb_malloc(slen + 1);
            for (size_t i = 0; i < slen; i++)
                up[i] = (char)toupper((unsigned char)src[i]);
            up[slen] = '\0';
            nvdb_val_set_str(&v, up, (uint32_t)slen);
        } else if (strcasecmp(e->fn.name, "LOWER") == 0 && e->fn.nargs > 0) {
            NVDBValue arg = eval_expr(e->fn.args[0], row, ncols, cols);
            const char *src = (arg.type == NVDB_TYPE_STRING)
                              ? (arg.str_val ? arg.str_val : "") : "";
            size_t slen = strlen(src);
            char *lo = nvdb_malloc(slen + 1);
            for (size_t i = 0; i < slen; i++)
                lo[i] = (char)tolower((unsigned char)src[i]);
            lo[slen] = '\0';
            nvdb_val_set_str(&v, lo, (uint32_t)slen);
        }
        break;

    default:
        break;
    }
    return v;
}

static bool eval_to_bool(ASTExpr *e, NVDBValue *row, int ncols,
                          ASTColumnDef *cols) {
    if (!e) return true;

    switch (e->kind) {
    case AST_LITERAL:
        if (e->lit.type == NVDB_TYPE_BOOL) return e->lit.bval;
        if (e->lit.type == NVDB_TYPE_INT64) return e->lit.i64 != 0;
        return e->lit.type != NVDB_TYPE_NULL;

    case AST_BINARY: {
        NVDBValue l = eval_expr(e->bin.left, row, ncols, cols);
        NVDBValue r = eval_expr(e->bin.right, row, ncols, cols);

        switch (e->bin.op) {
        case AST_OP_EQ:
            if (l.type == NVDB_TYPE_INT64 && r.type == NVDB_TYPE_INT64)
                return l.i64 == r.i64;
            if (l.type == NVDB_TYPE_STRING && r.type == NVDB_TYPE_STRING) {
                const char *ls = l.str_val ? l.str_val : "";
                const char *rs = r.str_val ? r.str_val : "";
                return strcmp(ls, rs) == 0;
            }
            return (l.type == NVDB_TYPE_NULL && r.type == NVDB_TYPE_NULL);
        case AST_OP_NEQ: {
            ASTExpr eqexpr = {
                .kind = AST_BINARY,
                .bin = {.op = AST_OP_EQ,
                        .left = e->bin.left, .right = e->bin.right}
            };
            return !eval_to_bool(&eqexpr, row, ncols, cols);
        }
        case AST_OP_LT:
            if (l.type == NVDB_TYPE_INT64 && r.type == NVDB_TYPE_INT64)
                return l.i64 < r.i64;
            return false;
        case AST_OP_GT:
            if (l.type == NVDB_TYPE_INT64 && r.type == NVDB_TYPE_INT64)
                return l.i64 > r.i64;
            return false;
        case AST_OP_LTE:
            if (l.type == NVDB_TYPE_INT64 && r.type == NVDB_TYPE_INT64)
                return l.i64 <= r.i64;
            return false;
        case AST_OP_GTE:
            if (l.type == NVDB_TYPE_INT64 && r.type == NVDB_TYPE_INT64)
                return l.i64 >= r.i64;
            return false;
        case AST_OP_AND:
            return eval_to_bool(e->bin.left, row, ncols, cols)
                && eval_to_bool(e->bin.right, row, ncols, cols);
        case AST_OP_OR:
            return eval_to_bool(e->bin.left, row, ncols, cols)
                || eval_to_bool(e->bin.right, row, ncols, cols);
        case AST_OP_IS:
            return (l.type == NVDB_TYPE_NULL) == (r.type == NVDB_TYPE_NULL);
        default:
            return true;
        }
    }

    case AST_UNARY:
        if (e->unary.op == AST_OP_NOT)
            return !eval_to_bool(e->unary.expr, row, ncols, cols);
        return false;

    default:
        return true;
    }
}

/* ── Main executor ────────────────────────────────────────────── */

ExecResult *executor_run(NVDBEngine *engine, ASTStmt *stmt) {
    ExecResult *res = nvdb_calloc(1, sizeof(*res));
    if (!stmt) { res->error = nvdb_strdup("null statement"); return res; }

    switch (stmt->kind) {

    /* ── CREATE TABLE ──────────────────────────────────────── */
    case AST_STMT_CREATE_TABLE: {
        if (cat_find(stmt->ct.table)) {
            res->error = nvdb_strdup(
                stmt->ct.if_not_exists ? "OK" : "table already exists");
            if (stmt->ct.if_not_exists) { free(res->error);
                res->error = NULL; res->message = nvdb_strdup("OK"); }
            return res;
        }
        cat_add(stmt->ct.table, stmt->ct.ncolumns, stmt->ct.columns);

        char key[256], val[2048];
        snprintf(key, sizeof(key), "__cat__:%s", stmt->ct.table);
        int vl = 0;
        for (int i = 0; i < stmt->ct.ncolumns; i++) {
            vl += snprintf(val + vl, sizeof(val) - (size_t)vl,
                           "%s,%s,%d,%d,%d;",
                           stmt->ct.columns[i].name,
                           stmt->ct.columns[i].type,
                           stmt->ct.columns[i].nullable ? 1 : 0,
                           stmt->ct.columns[i].primary ? 1 : 0,
                           stmt->ct.columns[i].unique ? 1 : 0);
        }
        engine_btree_insert(engine, key, (uint16_t)strlen(key),
                             val, (uint16_t)vl);

        char msg[256];
        snprintf(msg, sizeof(msg), "CREATE TABLE %s", stmt->ct.table);
        res->message = nvdb_strdup(msg);
        return res;
    }

    /* ── DROP TABLE ────────────────────────────────────────── */
    case AST_STMT_DROP_TABLE: {
        if (!cat_find(stmt->dt.table)) {
            if (stmt->dt.if_exists) {
                res->message = nvdb_strdup("table does not exist");
                return res;
            }
            res->error = nvdb_strdup("table does not exist");
            return res;
        }
        cat_remove(stmt->dt.table);

        char key[256];
        snprintf(key, sizeof(key), "__cat__:%s", stmt->dt.table);
        engine_btree_delete(engine, key, (uint16_t)strlen(key));

        char msg[256];
        snprintf(msg, sizeof(msg), "DROP TABLE %s", stmt->dt.table);
        res->message = nvdb_strdup(msg);
        return res;
    }

    /* ── INSERT ────────────────────────────────────────────── */
    case AST_STMT_INSERT: {
        CatalogEntry *cat = cat_find(stmt->ins.table);
        if (!cat) { res->error = nvdb_strdup("table not found"); return res; }

        int count = 0;
        for (int r = 0; r < stmt->ins.nvalues; r++) {
            ASTExpr *tuple = stmt->ins.values[r];
            int nv = (tuple && tuple->kind == AST_TUPLE)
                     ? tuple->tup.nexprs : 0;
            ASTExpr **vals = (tuple && tuple->kind == AST_TUPLE)
                             ? tuple->tup.exprs : NULL;

            char rowbuf[4096];
            int rowlen = encode_row(rowbuf, sizeof(rowbuf),
                                     cat->cols, cat->ncols,
                                     vals, nv,
                                     stmt->ins.col_names,
                                     stmt->ins.ncol_names);

            int64_t rid = engine_next_rowid(engine, stmt->ins.table);
            char key[256];
            snprintf(key, sizeof(key), "%s:%" PRId64,
                     stmt->ins.table, rid);

            engine_btree_insert(engine, key, (uint16_t)strlen(key),
                                 rowbuf, (uint16_t)rowlen);
            count++;
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "INSERT %d", count);
        res->message = nvdb_strdup(msg);
        res->nrows   = count;
        return res;
    }

    /* ── SELECT ────────────────────────────────────────────── */
    case AST_STMT_SELECT: {
        CatalogEntry *cat = cat_find(stmt->sel.table);
        if (!cat) { res->error = nvdb_strdup("table not found"); return res; }

        int        nmatch = 0;
        NVDBValue *matches[1024];

        /* Sequential scan by row ID (production code would use
           btree_scan with a prefix range) */
        for (int64_t rid = 1; ; rid++) {
            char key[256];
            snprintf(key, sizeof(key), "%s:%" PRId64,
                     stmt->sel.table, rid);

            char value[4096];
            uint16_t vlen;
            int rc = engine_btree_search(engine, key,
                                          (uint16_t)strlen(key),
                                          value, &vlen,
                                          sizeof(value) - 1);
            if (rc != NVDB_OK) break;
            value[vlen] = '\0';

            NVDBValue *row = nvdb_calloc((size_t)cat->ncols,
                                          sizeof(NVDBValue));
            decode_row(value, row, cat->ncols, cat->cols);

            if (stmt->sel.where &&
                !eval_to_bool(stmt->sel.where, row,
                              cat->ncols, cat->cols)) {
                /* Free string values in this row */
                for (int c = 0; c < cat->ncols; c++)
                    nvdb_val_free(&row[c]);
                free(row);
                continue;
            }

            if (nmatch < 1024) matches[nmatch++] = row;
            else {
                for (int c = 0; c < cat->ncols; c++)
                    nvdb_val_free(&row[c]);
                free(row);
            }
        }

        /* Determine output columns */
        bool is_star = (stmt->sel.ncolumns > 0
                        && stmt->sel.columns[0]->kind == AST_STAR);
        int out_ncols = is_star ? cat->ncols : stmt->sel.ncolumns;

        res->ncols = out_ncols;
        res->col_names = nvdb_calloc((size_t)out_ncols, sizeof(char *));
        for (int i = 0; i < out_ncols; i++) {
            if (is_star) {
                res->col_names[i] = nvdb_strdup(cat->cols[i].name);
            } else if (i < stmt->sel.ncolumns
                       && stmt->sel.columns[i]->kind == AST_COLUMN) {
                res->col_names[i] = nvdb_strdup(
                    stmt->sel.columns[i]->col.name);
            } else {
                char buf[32];
                snprintf(buf, sizeof(buf), "expr_%d", i);
                res->col_names[i] = nvdb_strdup(buf);
            }
        }

        res->nrows = nmatch;
        res->rows  = nvdb_calloc((size_t)nmatch, sizeof(NVDBValue *));
        for (int i = 0; i < nmatch; i++) {
            res->rows[i] = nvdb_calloc((size_t)out_ncols,
                                        sizeof(NVDBValue));
            for (int j = 0; j < out_ncols; j++) {
                if (is_star) {
                    /* Copy value — strings need deep copy */
                    NVDBValue *src = &matches[i][j];
                    res->rows[i][j] = *src;
                    if (src->type == NVDB_TYPE_STRING && src->str_val) {
                        res->rows[i][j].str_val
                            = nvdb_strdup(src->str_val);
                    }
                } else if (j < stmt->sel.ncolumns
                           && stmt->sel.columns[j]->kind == AST_COLUMN) {
                    for (int k = 0; k < cat->ncols; k++) {
                        if (strcasecmp(stmt->sel.columns[j]->col.name,
                                       cat->cols[k].name) == 0) {
                            NVDBValue *src = &matches[i][k];
                            res->rows[i][j] = *src;
                            if (src->type == NVDB_TYPE_STRING
                                && src->str_val) {
                                res->rows[i][j].str_val
                                    = nvdb_strdup(src->str_val);
                            }
                            break;
                        }
                    }
                }
            }
            /* Free source row */
            for (int c = 0; c < cat->ncols; c++)
                nvdb_val_free(&matches[i][c]);
            free(matches[i]);
        }

        return res;
    }

    /* ── DELETE ────────────────────────────────────────────── */
    case AST_STMT_DELETE: {
        CatalogEntry *cat = cat_find(stmt->del.table);
        if (!cat) { res->error = nvdb_strdup("table not found"); return res; }

        int count = 0;
        for (int64_t rid = 1; ; rid++) {
            char key[256];
            snprintf(key, sizeof(key), "%s:%" PRId64,
                     stmt->del.table, rid);

            char value[4096];
            uint16_t vlen;
            int rc = engine_btree_search(engine, key,
                                          (uint16_t)strlen(key),
                                          value, &vlen,
                                          sizeof(value) - 1);
            if (rc != NVDB_OK) break;
            value[vlen] = '\0';

            if (stmt->del.where) {
                NVDBValue *row = nvdb_calloc((size_t)cat->ncols,
                                              sizeof(NVDBValue));
                decode_row(value, row, cat->ncols, cat->cols);
                bool match = eval_to_bool(stmt->del.where, row,
                                          cat->ncols, cat->cols);
                for (int c = 0; c < cat->ncols; c++)
                    nvdb_val_free(&row[c]);
                free(row);
                if (!match) continue;
            }

            engine_btree_delete(engine, key, (uint16_t)strlen(key));
            count++;
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "DELETE %d", count);
        res->message = nvdb_strdup(msg);
        res->nrows   = count;
        return res;
    }

    /* ── UPDATE ────────────────────────────────────────────── */
    case AST_STMT_UPDATE: {
        CatalogEntry *cat = cat_find(stmt->upd.table);
        if (!cat) { res->error = nvdb_strdup("table not found"); return res; }

        int count = 0;
        for (int64_t rid = 1; ; rid++) {
            char key[256];
            snprintf(key, sizeof(key), "%s:%" PRId64,
                     stmt->upd.table, rid);

            char value[4096];
            uint16_t vlen;
            int rc = engine_btree_search(engine, key,
                                          (uint16_t)strlen(key),
                                          value, &vlen,
                                          sizeof(value) - 1);
            if (rc != NVDB_OK) break;
            value[vlen] = '\0';

            NVDBValue *row = nvdb_calloc((size_t)cat->ncols,
                                          sizeof(NVDBValue));
            decode_row(value, row, cat->ncols, cat->cols);

            if (stmt->upd.where &&
                !eval_to_bool(stmt->upd.where, row,
                              cat->ncols, cat->cols)) {
                for (int c = 0; c < cat->ncols; c++)
                    nvdb_val_free(&row[c]);
                free(row);
                continue;
            }

            /* Apply SET clauses */
            for (int s = 0; s < stmt->upd.nsets; s++) {
                for (int c = 0; c < cat->ncols; c++) {
                    if (strcasecmp(stmt->upd.sets[s].column,
                                   cat->cols[c].name) == 0) {
                        nvdb_val_free(&row[c]);
                        row[c] = eval_expr(stmt->upd.sets[s].value,
                                            row, cat->ncols, cat->cols);
                        break;
                    }
                }
            }

            /* Re-encode the row */
            char newval[4096];
            int newlen = 0;
            for (int c = 0; c < cat->ncols; c++) {
                const char *sv = "NULL";
                char tmp[64];
                switch (row[c].type) {
                case NVDB_TYPE_INT64:
                    snprintf(tmp, sizeof(tmp), "%" PRId64, row[c].i64);
                    sv = tmp; break;
                case NVDB_TYPE_STRING:
                    sv = row[c].str_val ? row[c].str_val : ""; break;
                case NVDB_TYPE_BOOL:
                    sv = row[c].bval ? "true" : "false"; break;
                case NVDB_TYPE_FLOAT64:
                    snprintf(tmp, sizeof(tmp), "%g", row[c].f64);
                    sv = tmp; break;
                default: break;
                }
                newlen += snprintf(newval + newlen,
                                    sizeof(newval) - (size_t)newlen,
                                    "%s=%s\n", cat->cols[c].name, sv);
            }

            engine_btree_insert(engine, key, (uint16_t)strlen(key),
                                 newval, (uint16_t)newlen);

            for (int c = 0; c < cat->ncols; c++)
                nvdb_val_free(&row[c]);
            free(row);
            count++;
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "UPDATE %d", count);
        res->message = nvdb_strdup(msg);
        res->nrows   = count;
        return res;
    }

    case AST_STMT_BEGIN:
        res->message = nvdb_strdup("BEGIN"); return res;
    case AST_STMT_COMMIT:
        res->message = nvdb_strdup("COMMIT"); return res;
    case AST_STMT_ROLLBACK:
        res->message = nvdb_strdup("ROLLBACK"); return res;

    default:
        res->error = nvdb_strdup("unsupported statement type");
        return res;
    }
}

void executor_free_result(ExecResult *res) {
    if (!res) return;
    free(res->message);
    free(res->error);
    if (res->col_names) {
        for (int i = 0; i < res->ncols; i++) free(res->col_names[i]);
        free(res->col_names);
    }
    if (res->rows) {
        for (int i = 0; i < res->nrows; i++) {
            for (int j = 0; j < res->ncols; j++)
                nvdb_val_free(&res->rows[i][j]);
            free(res->rows[i]);
        }
        free(res->rows);
    }
    free(res);
}
