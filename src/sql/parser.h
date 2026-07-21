/*
 * parser.h — SQL parser interface and AST definitions
 */
#ifndef NOVDB_PARSER_H
#define NOVDB_PARSER_H

#include "lexer.h"
#include "novadb/types.h"

/* ── AST nodes ────────────────────────────────────────────────── */

typedef enum {
    AST_OP_EQ, AST_OP_NEQ, AST_OP_LT, AST_OP_GT,
    AST_OP_LTE, AST_OP_GTE, AST_OP_AND, AST_OP_OR,
    AST_OP_ADD, AST_OP_SUB, AST_OP_MUL, AST_OP_DIV,
    AST_OP_NEG, AST_OP_NOT, AST_OP_LIKE, AST_OP_IS,
} ASTOp;

typedef enum {
    AST_LITERAL, AST_COLUMN, AST_BINARY, AST_UNARY,
    AST_FUNCTION, AST_STAR, AST_TUPLE,
} ASTExprKind;

typedef struct ASTExpr ASTExpr;
struct ASTExpr {
    ASTExprKind kind;
    union {
        struct { NVDBValue lit; }                  /* AST_LITERAL */
        struct { char *name; } col;                 /* AST_COLUMN   */
        struct { ASTOp op; ASTExpr *left, *right; } bin; /* AST_BINARY */
        struct { ASTOp op; ASTExpr *expr; } unary;   /* AST_UNARY   */
        struct { char *name; int nargs; ASTExpr **args; } fn; /* AST_FUNCTION */
        struct { int nexprs; ASTExpr **exprs; } tup;  /* AST_TUPLE    */
    };
};

typedef struct {
    char     *column;
    bool      desc;
} ASTOrderClause;

typedef struct {
    char     *column;
    ASTExpr  *value;
} ASTSetClause;

typedef struct {
    char     *name;
    char     *type;
    bool      nullable;
    bool      primary;
    bool      unique;
    bool      has_default;
    ASTExpr  *default_val;
} ASTColumnDef;

typedef enum {
    AST_STMT_SELECT, AST_STMT_INSERT, AST_STMT_UPDATE,
    AST_STMT_DELETE, AST_STMT_CREATE_TABLE, AST_STMT_DROP_TABLE,
    AST_STMT_BEGIN, AST_STMT_COMMIT, AST_STMT_ROLLBACK,
} ASTStmtKind;

typedef struct ASTStmt ASTStmt;
struct ASTStmt {
    ASTStmtKind kind;
    union {
        struct {
            ASTExpr  **columns; int ncolumns;
            char      *table;
            ASTExpr   *where;
            ASTOrderClause *orders; int norders;
            int        limit, offset;
            bool       distinct;
        } sel;
        struct {
            char      *table;
            char     **col_names; int ncol_names;
            ASTExpr  **values; int nvalues;
        } ins;
        struct {
            char      *table;
            ASTSetClause *sets; int nsets;
            ASTExpr   *where;
        } upd;
        struct {
            char      *table;
            ASTExpr   *where;
        } del;
        struct {
            char      *table;
            ASTColumnDef *columns; int ncolumns;
            bool       if_not_exists;
        } ct;
        struct {
            char      *table;
            bool       if_exists;
        } dt;
    };
};

/* ── Parser API ───────────────────────────────────────────────── */
void     parser_reset(void);
ASTStmt *parser_parse(SQLLexer *lex);

/* ── Helpers (exposed for executor) ───────────────────────────── */
ASTStmt *parser_make_stmt(ASTStmtKind kind);
ASTStmt *parser_parse_select(void *p);
ASTStmt *parser_parse_insert(void *p);
ASTStmt *parser_parse_update(void *p);
ASTStmt *parser_parse_delete(void *p);
ASTStmt *parser_parse_create(void *p);
ASTStmt *parser_parse_drop(void *p);

#endif /* NOVDB_PARSER_H */
