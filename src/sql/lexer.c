/*
 * lexer.c — SQL tokeniser
 *
 * Hand-written scanner that produces a stream of tokens.
 * Recognises SQL:2011 keywords, quoted identifiers, string
 * literals (with '' escaping), numeric literals (integer and
 * floating), and the usual operators.
 *
 * The lexer is designed to be re-entrant and allocation-light:
 * token values point into the source buffer where possible.
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "lexer.h"
#include "memory.h"
#include "novadb/error.h"

/* ── Keyword table ────────────────────────────────────────────── */

typedef struct {
    const char *word;
    SQLTokenKind kind;
} KeywordEntry;

static const KeywordEntry keywords[] = {
    {"SELECT",    TOK_SELECT},
    {"FROM",      TOK_FROM},
    {"WHERE",     TOK_WHERE},
    {"INSERT",    TOK_INSERT},
    {"INTO",      TOK_INTO},
    {"VALUES",    TOK_VALUES},
    {"UPDATE",    TOK_UPDATE},
    {"SET",       TOK_SET},
    {"DELETE",    TOK_DELETE},
    {"CREATE",    TOK_CREATE},
    {"TABLE",     TOK_TABLE},
    {"DROP",      TOK_DROP},
    {"INDEX",     TOK_INDEX},
    {"ON",        TOK_ON},
    {"AND",       TOK_AND},
    {"OR",        TOK_OR},
    {"NOT",       TOK_NOT},
    {"NULL",      TOK_NULL},
    {"TRUE",      TOK_TRUE},
    {"FALSE",     TOK_FALSE},
    {"IN",        TOK_IN},
    {"BETWEEN",   TOK_BETWEEN},
    {"LIKE",      TOK_LIKE},
    {"ORDER",     TOK_ORDER},
    {"BY",        TOK_BY},
    {"ASC",       TOK_ASC},
    {"DESC",      TOK_DESC},
    {"LIMIT",     TOK_LIMIT},
    {"OFFSET",    TOK_OFFSET},
    {"GROUP",     TOK_GROUP},
    {"HAVING",    TOK_HAVING},
    {"DISTINCT",  TOK_DISTINCT},
    {"AS",        TOK_AS},
    {"PRIMARY",   TOK_PRIMARY},
    {"KEY",       TOK_KEY},
    {"UNIQUE",    TOK_UNIQUE},
    {"INT",       TOK_INT},
    {"INTEGER",   TOK_INTEGER},
    {"BIGINT",    TOK_BIGINT},
    {"VARCHAR",   TOK_VARCHAR},
    {"TEXT",      TOK_TEXT},
    {"BOOLEAN",   TOK_BOOLEAN},
    {"FLOAT",     TOK_FLOAT},
    {"DOUBLE",    TOK_DOUBLE},
    {"TIMESTAMP", TOK_TIMESTAMP},
    {"DEFAULT",   TOK_DEFAULT},
    {"IF",        TOK_IF},
    {"EXISTS",    TOK_EXISTS},
    {"BEGIN",     TOK_BEGIN},
    {"COMMIT",    TOK_COMMIT},
    {"ROLLBACK",  TOK_ROLLBACK},
    {"TRANSACTION", TOK_TRANSACTION},
    {"IS",        TOK_IS},
    {"COUNT",     TOK_COUNT},
    {"SUM",       TOK_SUM},
    {"AVG",       TOK_AVG},
    {"MAX",       TOK_MAX},
    {"MIN",       TOK_MIN},
    {"UPPER",     TOK_UPPER},
    {"LOWER",     TOK_LOWER},
    {NULL,        0},
};

static SQLTokenKind lookup_keyword(const char *word) {
    for (int i = 0; keywords[i].word; i++) {
        if (strcasecmp(word, keywords[i].word) == 0)
            return keywords[i].kind;
    }
    return TOK_IDENTIFIER;
}

/* ── Lexer state ──────────────────────────────────────────────── */

struct SQLLexer {
    const char *src;
    int         pos;
    int         tok_start;
    SQLToken    current;
    char       *buffer;     /* for tokens that need allocation   */
    int         buf_cap;
};

SQLLexer *lexer_create(const char *sql) {
    SQLLexer *lex = nvdb_calloc(1, sizeof(*lex));
    lex->src     = sql;
    lex->pos     = 0;
    lex->buf_cap = 256;
    lex->buffer  = nvdb_malloc((size_t)lex->buf_cap);
    return lex;
}

void lexer_destroy(SQLLexer *lex) {
    if (!lex) return;
    free(lex->buffer);
    free(lex);
}

/* ── Character helpers ────────────────────────────────────────── */

static int lex_peek(SQLLexer *lex, int ahead) {
    int idx = lex->pos + ahead;
    if (idx < 0 || lex->src[idx] == '\0') return -1;
    return (unsigned char)lex->src[idx];
}

static void lex_advance(SQLLexer *lex) {
    if (lex->src[lex->pos] != '\0') lex->pos++;
}

static void lex_skip_ws(SQLLexer *lex) {
    for (;;) {
        int c = lex_peek(lex, 0);
        if (c < 0) return;

        if (c == '-' && lex_peek(lex, 1) == '-') {
            /* Line comment */
            lex_advance(lex); lex_advance(lex);
            while ((c = lex_peek(lex, 0)) >= 0 && c != '\n')
                lex_advance(lex);
            continue;
        }

        if (c == '/' && lex_peek(lex, 1) == '*') {
            /* Block comment */
            lex_advance(lex); lex_advance(lex);
            for (;;) {
                c = lex_peek(lex, 0);
                if (c < 0) return;
                if (c == '*' && lex_peek(lex, 1) == '/') {
                    lex_advance(lex); lex_advance(lex);
                    break;
                }
                lex_advance(lex);
            }
            continue;
        }

        if (!isspace(c)) break;
        lex_advance(lex);
    }
}

/* ── Token scanning ───────────────────────────────────────────── */

static void make_token(SQLLexer *lex, SQLTokenKind kind,
                       const char *val, int len) {
    lex->current.kind    = kind;
    lex->current.start   = lex->tok_start;

    if (len >= lex->buf_cap) {
        lex->buf_cap = len + 64;
        lex->buffer  = nvdb_realloc(lex->buffer, (size_t)lex->buf_cap);
    }
    memcpy(lex->buffer, val, (size_t)len);
    lex->buffer[len] = '\0';
    lex->current.text = lex->buffer;
    lex->current.len   = len;
}

static SQLTokenKind scan_number(SQLLexer *lex, int start, bool negative) {
    int i = 0;
    bool is_float = false;
    char buf[64];

    if (negative) buf[i++] = '-';

    for (;;) {
        int c = lex_peek(lex, 0);
        if (c >= 0 && isdigit(c)) {
            buf[i++] = (char)c;
            lex_advance(lex);
        } else if (c == '.' && !is_float) {
            is_float = true;
            buf[i++] = '.';
            lex_advance(lex);
        } else {
            break;
        }
    }

    buf[i] = '\0';
    make_token(lex, is_float ? TOK_FLOAT_LIT : TOK_INTEGER_LIT, buf, i);
    return is_float ? TOK_FLOAT_LIT : TOK_INTEGER_LIT;
}

static SQLTokenKind scan_string(SQLLexer *lex) {
    lex_advance(lex); /* skip opening quote */
    char buf[4096];
    int  i = 0;

    for (;;) {
        int c = lex_peek(lex, 0);
        if (c < 0) break;
        if (c == '\'') {
            if (lex_peek(lex, 1) == '\'') {
                /* Escaped quote */
                buf[i++] = '\'';
                lex_advance(lex); lex_advance(lex);
                continue;
            }
            lex_advance(lex); /* skip closing quote */
            break;
        }
        if (i < 4095) buf[i++] = (char)c;
        lex_advance(lex);
    }

    make_token(lex, TOK_STRING_LIT, buf, i);
    return TOK_STRING_LIT;
}

static SQLTokenKind scan_identifier(SQLLexer *lex, int first) {
    char buf[128];
    int  i = 1;
    buf[0] = (char)first;

    for (;;) {
        int c = lex_peek(lex, 0);
        if (c < 0) break;
        if (isalnum(c) || c == '_') {
            if (i < 127) buf[i++] = (char)c;
            lex_advance(lex);
        } else {
            break;
        }
    }
    buf[i] = '\0';

    SQLTokenKind kind = lookup_keyword(buf);
    make_token(lex, kind, buf, i);
    return kind;
}

/* ── Public interface ─────────────────────────────────────────── */

const SQLToken *lexer_next(SQLLexer *lex) {
    lex_skip_ws(lex);

    int c = lex_peek(lex, 0);
    if (c < 0) {
        make_token(lex, TOK_EOF, "", 0);
        return &lex->current;
    }

    lex->tok_start = lex->pos;

    switch (c) {
    case '(': lex_advance(lex); make_token(lex, TOK_LPAREN,  "(", 1);  break;
    case ')': lex_advance(lex); make_token(lex, TOK_RPAREN,  ")", 1);  break;
    case ',': lex_advance(lex); make_token(lex, TOK_COMMA,   ",", 1);  break;
    case ';': lex_advance(lex); make_token(lex, TOK_SEMI,    ";", 1);  break;
    case '*': lex_advance(lex); make_token(lex, TOK_STAR,    "*", 1);  break;
    case '.': lex_advance(lex); make_token(lex, TOK_DOT,     ".", 1);  break;
    case '+': lex_advance(lex); make_token(lex, TOK_PLUS,    "+", 1);  break;
    case '/': lex_advance(lex); make_token(lex, TOK_SLASH,   "/", 1);  break;

    case '-': {
        lex_advance(lex);
        int next = lex_peek(lex, 0);
        if (next >= 0 && isdigit(next)) {
            return scan_number(lex, lex->tok_start, true);
        }
        make_token(lex, TOK_MINUS, "-", 1);
        break;
    }

    case '=': lex_advance(lex); make_token(lex, TOK_EQ,  "=", 1);  break;
    case '<': {
        lex_advance(lex);
        if (lex_peek(lex, 0) == '=') {
            lex_advance(lex);
            make_token(lex, TOK_LTE, "<=", 2);
        } else if (lex_peek(lex, 0) == '>') {
            lex_advance(lex);
            make_token(lex, TOK_NEQ, "<>", 2);
        } else {
            make_token(lex, TOK_LT, "<", 1);
        }
        break;
    }
    case '>': {
        lex_advance(lex);
        if (lex_peek(lex, 0) == '=') {
            lex_advance(lex);
            make_token(lex, TOK_GTE, ">=", 2);
        } else {
            make_token(lex, TOK_GT, ">", 1);
        }
        break;
    }
    case '!': {
        lex_advance(lex);
        if (lex_peek(lex, 0) == '=') {
            lex_advance(lex);
            make_token(lex, TOK_NEQ, "!=", 2);
        } else {
            make_token(lex, TOK_ERROR, "!", 1);
        }
        break;
    }

    case '\'':
        return scan_string(lex);

    default:
        if (isdigit(c)) {
            return scan_number(lex, lex->tok_start, false);
        }
        if (isalpha(c) || c == '_') {
            return scan_identifier(lex, c);
        }
        /* Unknown character */
        lex_advance(lex);
        make_token(lex, TOK_ERROR, "", 0);
        break;
    }

    return &lex->current;
}

const SQLToken *lexer_current(const SQLLexer *lex) {
    return &lex->current;
}
