/*
 * parser.c — Recursive-descent SQL parser
 *
 * Parses a token stream into an Abstract Syntax Tree.
 * Grammar is a strict subset of SQL:2011 covering:
 *   SELECT ... FROM ... WHERE ... ORDER BY ... LIMIT
 *   INSERT INTO ... VALUES ...
 *   UPDATE ... SET ... WHERE ...
 *   DELETE FROM ... WHERE ...
 *   CREATE TABLE ... (...)
 *   DROP TABLE ...
 *   BEGIN / COMMIT / ROLLBACK
 *
 * Expression parsing uses Pratt-style precedence climbing.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "parser.h"
#include "lexer.h"
#include "memory.h"
#include "novadb/error.h"

/* ── AST node allocator ───────────────────────────────────────── */

static NVDBArena *g_arena = NULL;

static void *ast_alloc(size_t sz) {
    if (!g_arena) g_arena = nvdb_arena_create();
    return nvdb_arena_alloc(g_arena, sz);
}

void parser_reset(void) {
    if (g_arena) nvdb_arena_reset(g_arena);
}

/* ── Parser state ─────────────────────────────────────────────── */

typedef struct {
    SQLLexer   *lex;
    const SQLToken *tok;
} Parser;

static void p_advance(Parser *p) {
    p->tok = lexer_next(p->lex);
}

static bool p_match(Parser *p, SQLTokenKind k) {
    return p->tok->kind == k;
}

static bool p_eat(Parser *p, SQLTokenKind k) {
    if (p->tok->kind == k) {
        p_advance(p);
        return true;
    }
    return false;
}

static bool p_expect(Parser *p, SQLTokenKind k) {
    if (p->tok->kind == k) {
        p_advance(p);
        return true;
    }
    nvdb_set_error(NVDB_ERR_PARSE,
                   "expected %d, got %d ('%s') at pos %d",
                   k, p->tok->kind, p->tok->text, p->tok->start);
    return false;
}

/* ── Expression parsing (Pratt) ───────────────────────────────── */

static ASTExpr *p_expr(Parser *p, int min_prec);

static int prec_of(SQLTokenKind k) {
    switch (k) {
    case TOK_OR:       return 1;
    case TOK_AND:      return 2;
    case TOK_NOT:      return 3;
    case TOK_EQ: case TOK_NEQ: case TOK_LT:
    case TOK_GT: case TOK_LTE: case TOK_GTE:
    case TOK_LIKE: case TOK_IN: case TOK_BETWEEN:
    case TOK_IS:       return 4;
    case TOK_PLUS: case TOK_MINUS: return 5;
    case TOK_STAR: case TOK_SLASH: return 6;
    default:           return 0;
    }
}

static ASTExpr *p_primary(Parser *p) {
    ASTExpr *e;

    switch (p->tok->kind) {
    case TOK_INTEGER_LIT: {
        e = ast_alloc(sizeof(*e));
        e->kind = AST_LITERAL;
        e->lit.type = NVDB_TYPE_INT64;
        e->lit.i64 = strtoll(p->tok->text, NULL, 10);
        p_advance(p);
        return e;
    }
    case TOK_FLOAT_LIT: {
        e = ast_alloc(sizeof(*e));
        e->kind = AST_LITERAL;
        e->lit.type = NVDB_TYPE_FLOAT64;
        e->lit.f64 = strtod(p->tok->text, NULL);
        p_advance(p);
        return e;
    }
    case TOK_STRING_LIT: {
        e = ast_alloc(sizeof(*e));
        e->kind = AST_LITERAL;
        e->lit.type = NVDB_TYPE_STRING;
        e->lit.str_val = nvdb_arena_strdup(g_arena, p->tok->text);
        p_advance(p);
        return e;
    }
    case TOK_NULL: {
        e = ast_alloc(sizeof(*e));
        e->kind = AST_LITERAL;
        e->lit.type = NVDB_TYPE_NULL;
        p_advance(p);
        return e;
    }
    case TOK_TRUE: case TOK_FALSE: {
        e = ast_alloc(sizeof(*e));
        e->kind = AST_LITERAL;
        e->lit.type = NVDB_TYPE_BOOL;
        e->lit.bval = (p->tok->kind == TOK_TRUE);
        p_advance(p);
        return e;
    }
    case TOK_IDENTIFIER: {
        e = ast_alloc(sizeof(*e));
        e->kind = AST_COLUMN;
        e->col.name = nvdb_arena_strdup(g_arena, p->tok->text);
        p_advance(p);

        /* Function call? */
        if (p_match(p, TOK_LPAREN)) {
            ASTExpr *fn = ast_alloc(sizeof(*fn));
            fn->kind = AST_FUNCTION;
            fn->fn.name = e->col.name;
            fn->fn.nargs = 0;
            fn->fn.args = NULL;
            p_advance(p);

            if (!p_match(p, TOK_RPAREN)) {
                /* Parse arguments */
                ASTExpr *args[16];
                int nargs = 0;
                do {
                    if (nargs < 16)
                        args[nargs++] = p_expr(p, 0);
                    else
                        p_expr(p, 0);
                } while (p_eat(p, TOK_COMMA));

                fn->fn.nargs = nargs;
                fn->fn.args = ast_alloc(sizeof(ASTExpr *) * (size_t)nargs);
                memcpy(fn->fn.args, args, sizeof(ASTExpr *) * (size_t)nargs);
            }
            p_expect(p, TOK_RPAREN);
            return fn;
        }
        return e;
    }
    case TOK_STAR: {
        e = ast_alloc(sizeof(*e));
        e->kind = AST_STAR;
        p_advance(p);
        return e;
    }
    case TOK_LPAREN: {
        p_advance(p);
        e = p_expr(p, 0);
        p_expect(p, TOK_RPAREN);
        return e;
    }
    default:
        nvdb_set_error(NVDB_ERR_PARSE,
                       "unexpected token '%s' in expression", p->tok->text);
        return NULL;
    }
}

static ASTExpr *p_expr(Parser *p, int min_prec) {
    ASTExpr *left;

    /* Prefix operators */
    if (p_match(p, TOK_MINUS)) {
        p_advance(p);
        ASTExpr *inner = p_expr(p, 6);
        left = ast_alloc(sizeof(*left));
        left->kind = AST_UNARY;
        left->unary.op = AST_OP_NEG;
        left->unary.expr = inner;
    } else if (p_match(p, TOK_NOT)) {
        p_advance(p);
        ASTExpr *inner = p_expr(p, 3);
        left = ast_alloc(sizeof(*left));
        left->kind = AST_UNARY;
        left->unary.op = AST_OP_NOT;
        left->unary.expr = inner;
    } else {
        left = p_primary(p);
        if (!left) return NULL;
    }

    for (;;) {
        int prec = prec_of(p->tok->kind);
        if (prec == 0 || prec < min_prec) break;

        SQLTokenKind op_kind = p->tok->kind;
        p_advance(p);

        ASTExpr *right = p_expr(p, prec);
        if (!right) return left;

        ASTExpr *bin = ast_alloc(sizeof(*bin));
        bin->kind = AST_BINARY;
        switch (op_kind) {
        case TOK_EQ:  bin->bin.op = AST_OP_EQ;  break;
        case TOK_NEQ: bin->bin.op = AST_OP_NEQ; break;
        case TOK_LT:  bin->bin.op = AST_OP_LT;  break;
        case TOK_GT:  bin->bin.op = AST_OP_GT;  break;
        case TOK_LTE: bin->bin.op = AST_OP_LTE; break;
        case TOK_GTE: bin->bin.op = AST_OP_GTE; break;
        case TOK_AND: bin->bin.op = AST_OP_AND; break;
        case TOK_OR:  bin->bin.op = AST_OP_OR;  break;
        case TOK_PLUS:  bin->bin.op = AST_OP_ADD; break;
        case TOK_MINUS: bin->bin.op = AST_OP_SUB; break;
        case TOK_STAR:  bin->bin.op = AST_OP_MUL; break;
        case TOK_SLASH: bin->bin.op = AST_OP_DIV; break;
        case TOK_LIKE:  bin->bin.op = AST_OP_LIKE; break;
        default: break;
        }
        bin->bin.left  = left;
        bin->bin.right = right;
        left = bin;
    }

    return left;
}

/* ── Statement parsing ────────────────────────────────────────── */

ASTStmt *parser_parse(SQLLexer *lex) {
    Parser p;
    p.lex = lex;
    p_advance(&p);

    if (p_match(&p, TOK_EOF)) return NULL;

    switch (p.tok->kind) {
    case TOK_SELECT:  return parser_parse_select(&p);
    case TOK_INSERT:  return parser_parse_insert(&p);
    case TOK_UPDATE:  return parser_parse_update(&p);
    case TOK_DELETE:  return parser_parse_delete(&p);
    case TOK_CREATE:  return parser_parse_create(&p);
    case TOK_DROP:    return parser_parse_drop(&p);
    case TOK_BEGIN:   p_advance(&p); p_eat(&p, TOK_TRANSACTION);
                      return parser_make_stmt(AST_STMT_BEGIN);
    case TOK_COMMIT:  p_advance(&p); p_eat(&p, TOK_TRANSACTION);
                      return parser_make_stmt(AST_STMT_COMMIT);
    case TOK_ROLLBACK: p_advance(&p); p_eat(&p, TOK_TRANSACTION);
                      return parser_make_stmt(AST_STMT_ROLLBACK);
    default:
        nvdb_set_error(NVDB_ERR_PARSE,
                       "unexpected token '%s' at start of statement",
                       p.tok->text);
        return NULL;
    }
}

ASTStmt *parser_make_stmt(ASTStmtKind kind) {
    ASTStmt *s = ast_alloc(sizeof(*s));
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    return s;
}

/* ── SELECT ───────────────────────────────────────────────────── */

ASTStmt *parser_parse_select(Parser *p) {
    p_advance(p); /* SELECT */

    ASTStmt *s = parser_make_stmt(AST_STMT_SELECT);
    int ncols = 0;

    /* DISTINCT */
    if (p_eat(p, TOK_DISTINCT))
        s->sel.distinct = true;

    /* Column list */
    if (p_match(p, TOK_STAR)) {
        s->sel.columns = ast_alloc(sizeof(ASTExpr *));
        ASTExpr *star = ast_alloc(sizeof(*star));
        star->kind = AST_STAR;
        s->sel.columns[0] = star;
        s->sel.ncolumns = 1;
        p_advance(p);
    } else {
        ASTExpr *cols[64];
        do {
            ASTExpr *e = p_expr(p, 0);
            if (!e) return NULL;
            if (ncols < 64) cols[ncols++] = e;
        } while (p_eat(p, TOK_COMMA));

        s->sel.columns = ast_alloc(sizeof(ASTExpr *) * (size_t)ncols);
        memcpy(s->sel.columns, cols, sizeof(ASTExpr *) * (size_t)ncols);
        s->sel.ncolumns = ncols;
    }

    /* FROM */
    if (p_eat(p, TOK_FROM)) {
        if (!p_expect(p, TOK_IDENTIFIER)) return NULL;
        s->sel.table = nvdb_arena_strdup(g_arena, p->tok->text);
        p_advance(p);
    }

    /* WHERE */
    if (p_eat(p, TOK_WHERE)) {
        s->sel.where = p_expr(p, 0);
    }

    /* ORDER BY */
    if (p_eat(p, TOK_ORDER)) {
        if (!p_expect(p, TOK_BY)) return NULL;

        ASTOrderClause orders[16];
        int norders = 0;
        do {
            if (!p_expect(p, TOK_IDENTIFIER)) return NULL;
            orders[norders].column = nvdb_arena_strdup(g_arena, p->tok->text);
            orders[norders].desc = false;
            p_advance(p);

            if (p_match(p, TOK_ASC))  p_advance(p);
            else if (p_match(p, TOK_DESC)) {
                orders[norders].desc = true;
                p_advance(p);
            }
            norders++;
        } while (p_eat(p, TOK_COMMA));

        s->sel.norders = norders;
        s->sel.orders  = ast_alloc(sizeof(ASTOrderClause) * (size_t)norders);
        memcpy(s->sel.orders, orders, sizeof(ASTOrderClause) * (size_t)norders);
    }

    /* LIMIT */
    if (p_eat(p, TOK_LIMIT)) {
        if (!p_expect(p, TOK_INTEGER_LIT)) return NULL;
        s->sel.limit = (int)strtol(p->tok->text, NULL, 10);
        p_advance(p);
    }

    /* OFFSET */
    if (p_eat(p, TOK_OFFSET)) {
        if (!p_expect(p, TOK_INTEGER_LIT)) return NULL;
        s->sel.offset = (int)strtol(p->tok->text, NULL, 10);
        p_advance(p);
    }

    return s;
}

/* ── INSERT ───────────────────────────────────────────────────── */

ASTStmt *parser_parse_insert(Parser *p) {
    p_advance(p); /* INSERT */
    if (!p_expect(p, TOK_INTO)) return NULL;

    ASTStmt *s = parser_make_stmt(AST_STMT_INSERT);

    if (!p_expect(p, TOK_IDENTIFIER)) return NULL;
    s->ins.table = nvdb_arena_strdup(g_arena, p->tok->text);
    p_advance(p);

    /* Optional column list */
    if (p_match(p, TOK_LPAREN)) {
        p_advance(p);
        char *cols[64];
        int  n = 0;
        do {
            if (!p_expect(p, TOK_IDENTIFIER)) return NULL;
            cols[n++] = nvdb_arena_strdup(g_arena, p->tok->text);
            p_advance(p);
        } while (p_eat(p, TOK_COMMA));
        p_expect(p, TOK_RPAREN);
        s->ins.col_names  = ast_alloc(sizeof(char *) * (size_t)n);
        memcpy(s->ins.col_names, cols, sizeof(char *) * (size_t)n);
        s->ins.ncol_names = n;
    }

    if (!p_expect(p, TOK_VALUES)) return NULL;

    /* Value rows */
    ASTExpr *rows[64];
    ASTExpr *row_vals[64];
    int nrows = 0;
    do {
        if (!p_expect(p, TOK_LPAREN)) return NULL;

        int nv = 0;
        do {
            ASTExpr *val = p_expr(p, 0);
            if (!val) return NULL;
            if (nv < 64) row_vals[nv++] = val;
        } while (p_eat(p, TOK_COMMA));
        p_expect(p, TOK_RPAREN);

        /* Make a tuple expression */
        ASTExpr *tuple = ast_alloc(sizeof(*tuple));
        tuple->kind = AST_TUPLE;
        tuple->tup.nexprs = nv;
        tuple->tup.exprs  = ast_alloc(sizeof(ASTExpr *) * (size_t)nv);
        memcpy(tuple->tup.exprs, row_vals, sizeof(ASTExpr *) * (size_t)nv);

        if (nrows < 64) rows[nrows++] = tuple;
    } while (p_eat(p, TOK_COMMA));

    s->ins.nvalues = nrows;
    s->ins.values  = ast_alloc(sizeof(ASTExpr *) * (size_t)nrows);
    memcpy(s->ins.values, rows, sizeof(ASTExpr *) * (size_t)nrows);

    return s;
}

/* ── UPDATE ───────────────────────────────────────────────────── */

ASTStmt *parser_parse_update(Parser *p) {
    p_advance(p); /* UPDATE */

    ASTStmt *s = parser_make_stmt(AST_STMT_UPDATE);

    if (!p_expect(p, TOK_IDENTIFIER)) return NULL;
    s->upd.table = nvdb_arena_strdup(g_arena, p->tok->text);
    p_advance(p);

    if (!p_expect(p, TOK_SET)) return NULL;

    ASTSetClause sets[64];
    int nsets = 0;
    do {
        if (!p_expect(p, TOK_IDENTIFIER)) return NULL;
        sets[nsets].column = nvdb_arena_strdup(g_arena, p->tok->text);
        p_advance(p);

        if (!p_expect(p, TOK_EQ)) return NULL;
        sets[nsets].value = p_expr(p, 0);
        nsets++;
    } while (p_eat(p, TOK_COMMA));

    s->upd.nsets = nsets;
    s->upd.sets  = ast_alloc(sizeof(ASTSetClause) * (size_t)nsets);
    memcpy(s->upd.sets, sets, sizeof(ASTSetClause) * (size_t)nsets);

    if (p_eat(p, TOK_WHERE)) {
        s->upd.where = p_expr(p, 0);
    }

    return s;
}

/* ── DELETE ───────────────────────────────────────────────────── */

ASTStmt *parser_parse_delete(Parser *p) {
    p_advance(p); /* DELETE */
    if (!p_expect(p, TOK_FROM)) return NULL;

    ASTStmt *s = parser_make_stmt(AST_STMT_DELETE);

    if (!p_expect(p, TOK_IDENTIFIER)) return NULL;
    s->del.table = nvdb_arena_strdup(g_arena, p->tok->text);
    p_advance(p);

    if (p_eat(p, TOK_WHERE)) {
        s->del.where = p_expr(p, 0);
    }

    return s;
}

/* ── CREATE TABLE ─────────────────────────────────────────────── */

ASTStmt *parser_parse_create(Parser *p) {
    p_advance(p); /* CREATE */

    if (!p_expect(p, TOK_TABLE)) return NULL;

    ASTStmt *s = parser_make_stmt(AST_STMT_CREATE_TABLE);

    /* IF NOT EXISTS */
    if (p_eat(p, TOK_IF)) {
        if (!p_expect(p, TOK_NOT)) return NULL;
        if (!p_expect(p, TOK_EXISTS)) return NULL;
        s->ct.if_not_exists = true;
    }

    if (!p_expect(p, TOK_IDENTIFIER)) return NULL;
    s->ct.table = nvdb_arena_strdup(g_arena, p->tok->text);
    p_advance(p);

    if (!p_expect(p, TOK_LPAREN)) return NULL;

    ASTColumnDef cols[64];
    int ncols = 0;
    do {
        if (!p_expect(p, TOK_IDENTIFIER)) return NULL;
        cols[ncols].name = nvdb_arena_strdup(g_arena, p->tok->text);
        p_advance(p);

        /* Type */
        cols[ncols].type = nvdb_arena_strdup(g_arena, p->tok->text);
        if (!(p->tok->kind >= TOK_INT && p->tok->kind <= TOK_TIMESTAMP)) {
            nvdb_set_error(NVDB_ERR_PARSE, "expected type for column '%s'",
                           cols[ncols].name);
            return NULL;
        }
        p_advance(p);

        cols[ncols].nullable  = true;
        cols[ncols].primary   = false;
        cols[ncols].unique    = false;
        cols[ncols].has_default = false;

        /* Constraints */
        while (p->tok->kind == TOK_PRIMARY || p->tok->kind == TOK_NOT ||
               p->tok->kind == TOK_NULL || p->tok->kind == TOK_UNIQUE ||
               p->tok->kind == TOK_DEFAULT) {
            if (p_eat(p, TOK_PRIMARY)) {
                p_expect(p, TOK_KEY);
                cols[ncols].primary  = true;
                cols[ncols].nullable = false;
            } else if (p_eat(p, TOK_NOT)) {
                if (p->tok->kind == TOK_NULL) p_advance(p);
                cols[ncols].nullable = false;
            } else if (p_eat(p, TOK_NULL)) {
                cols[ncols].nullable = true;
            } else if (p_eat(p, TOK_UNIQUE)) {
                cols[ncols].unique = true;
            } else if (p_eat(p, TOK_DEFAULT)) {
                cols[ncols].default_val = p_expr(p, 0);
                cols[ncols].has_default = true;
            } else {
                break;
            }
        }

        ncols++;
    } while (p_eat(p, TOK_COMMA));

    p_expect(p, TOK_RPAREN);

    s->ct.ncolumns = ncols;
    s->ct.columns  = ast_alloc(sizeof(ASTColumnDef) * (size_t)ncols);
    memcpy(s->ct.columns, cols, sizeof(ASTColumnDef) * (size_t)ncols);

    return s;
}

/* ── DROP TABLE ───────────────────────────────────────────────── */

ASTStmt *parser_parse_drop(Parser *p) {
    p_advance(p); /* DROP */
    if (!p_expect(p, TOK_TABLE)) return NULL;

    ASTStmt *s = parser_make_stmt(AST_STMT_DROP_TABLE);

    if (p_eat(p, TOK_IF)) {
        if (!p_expect(p, TOK_EXISTS)) return NULL;
        s->dt.if_exists = true;
    }

    if (!p_expect(p, TOK_IDENTIFIER)) return NULL;
    s->dt.table = nvdb_arena_strdup(g_arena, p->tok->text);
    p_advance(p);

    return s;
}
