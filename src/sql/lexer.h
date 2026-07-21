/*
 * lexer.h — SQL lexer interface
 */
#ifndef NOVDB_LEXER_H
#define NOVDB_LEXER_H

#include <stdbool.h>

/* ── Token kinds ──────────────────────────────────────────────── */
typedef enum {
    TOK_EOF = 0,
    TOK_ERROR,

    /* Keywords */
    TOK_SELECT, TOK_FROM, TOK_WHERE, TOK_INSERT, TOK_INTO,
    TOK_VALUES, TOK_UPDATE, TOK_SET, TOK_DELETE, TOK_CREATE,
    TOK_TABLE, TOK_DROP, TOK_INDEX, TOK_ON,
    TOK_AND, TOK_OR, TOK_NOT, TOK_NULL, TOK_TRUE, TOK_FALSE,
    TOK_IN, TOK_BETWEEN, TOK_LIKE, TOK_IS,
    TOK_ORDER, TOK_BY, TOK_ASC, TOK_DESC,
    TOK_LIMIT, TOK_OFFSET,
    TOK_GROUP, TOK_HAVING, TOK_DISTINCT, TOK_AS,
    TOK_PRIMARY, TOK_KEY, TOK_UNIQUE,
    TOK_INT, TOK_INTEGER, TOK_BIGINT, TOK_VARCHAR, TOK_TEXT,
    TOK_BOOLEAN, TOK_FLOAT, TOK_DOUBLE, TOK_TIMESTAMP,
    TOK_DEFAULT, TOK_IF, TOK_EXISTS,
    TOK_BEGIN, TOK_COMMIT, TOK_ROLLBACK, TOK_TRANSACTION,
    TOK_COUNT, TOK_SUM, TOK_AVG, TOK_MAX, TOK_MIN,
    TOK_UPPER, TOK_LOWER,

    /* Identifiers and literals */
    TOK_IDENTIFIER,
    TOK_STRING_LIT,
    TOK_INTEGER_LIT,
    TOK_FLOAT_LIT,

    /* Operators and punctuation */
    TOK_LPAREN, TOK_RPAREN,
    TOK_COMMA, TOK_SEMI, TOK_DOT, TOK_STAR,
    TOK_PLUS, TOK_MINUS, TOK_SLASH,
    TOK_EQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LTE, TOK_GTE,
} SQLTokenKind;

/* ── Token structure ──────────────────────────────────────────── */
typedef struct {
    SQLTokenKind kind;
    const char  *text;
    int          len;
    int          start;       /* offset in source                  */
} SQLToken;

/* ── Lexer ────────────────────────────────────────────────────── */
typedef struct SQLLexer SQLLexer;

SQLLexer      *lexer_create(const char *sql);
void           lexer_destroy(SQLLexer *lex);
const SQLToken *lexer_next(SQLLexer *lex);
const SQLToken *lexer_current(const SQLLexer *lex);

#endif /* NOVDB_LEXER_H */
