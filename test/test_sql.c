/*
 * test_sql.c — SQL lexer and parser tests
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "sql/lexer.h"
#include "sql/parser.h"

static int test_lexer_basic(void) {
    SQLLexer *lex = lexer_create("SELECT * FROM users WHERE id = 1");
    assert(lex != NULL);

    const SQLToken *t;

    t = lexer_next(lex);
    assert(t->kind == TOK_SELECT);

    t = lexer_next(lex);
    assert(t->kind == TOK_STAR);

    t = lexer_next(lex);
    assert(t->kind == TOK_FROM);

    t = lexer_next(lex);
    assert(t->kind == TOK_IDENTIFIER);
    assert(strcmp(t->text, "users") == 0);

    t = lexer_next(lex);
    assert(t->kind == TOK_WHERE);

    t = lexer_next(lex);
    assert(t->kind == TOK_IDENTIFIER);
    assert(strcmp(t->text, "id") == 0);

    t = lexer_next(lex);
    assert(t->kind == TOK_EQ);

    t = lexer_next(lex);
    assert(t->kind == TOK_INTEGER_LIT);

    t = lexer_next(lex);
    assert(t->kind == TOK_EOF);

    lexer_destroy(lex);
    return 0;
}

static int test_parser_select(void) {
    SQLLexer *lex = lexer_create("SELECT name, age FROM users WHERE age >= 18");
    assert(lex != NULL);

    parser_reset();
    ASTStmt *stmt = parser_parse(lex);
    assert(stmt != NULL);
    assert(stmt->kind == AST_STMT_SELECT);
    assert(stmt->sel.ncolumns == 2);
    assert(strcmp(stmt->sel.table, "users") == 0);
    assert(stmt->sel.where != NULL);

    lexer_destroy(lex);
    return 0;
}

static int test_parser_insert(void) {
    SQLLexer *lex = lexer_create(
        "INSERT INTO users (id, name) VALUES (1, 'Alice')");
    assert(lex != NULL);

    parser_reset();
    ASTStmt *stmt = parser_parse(lex);
    assert(stmt != NULL);
    assert(stmt->kind == AST_STMT_INSERT);
    assert(strcmp(stmt->ins.table, "users") == 0);
    assert(stmt->ins.nvalues == 1);
    assert(stmt->ins.ncol_names == 2);

    lexer_destroy(lex);
    return 0;
}

static int test_parser_create_table(void) {
    SQLLexer *lex = lexer_create(
        "CREATE TABLE test ("
        "  id INTEGER PRIMARY KEY,"
        "  name VARCHAR(255) NOT NULL,"
        "  score FLOAT DEFAULT 0.0"
        ")");
    assert(lex != NULL);

    parser_reset();
    ASTStmt *stmt = parser_parse(lex);
    assert(stmt != NULL);
    assert(stmt->kind == AST_STMT_CREATE_TABLE);
    assert(strcmp(stmt->ct.table, "test") == 0);
    assert(stmt->ct.ncolumns == 3);
    assert(stmt->ct.columns[0].primary == true);
    assert(stmt->ct.columns[1].nullable == false);
    assert(stmt->ct.columns[2].has_default == true);

    lexer_destroy(lex);
    return 0;
}

int test_sql(void) {
    int failures = 0;

    if (test_lexer_basic() != 0) failures++;
    if (test_parser_select() != 0) failures++;
    if (test_parser_insert() != 0) failures++;
    if (test_parser_create_table() != 0) failures++;

    return failures;
}
