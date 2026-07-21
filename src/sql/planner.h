/*
 * planner.h — SQL query planner interface
 *
 * Declares the entry point for the query planning phase that sits
 * between the parser and executor. Currently a pass-through while
 * full cost-based optimization is under development.
 */
#ifndef NOVDB_PLANNER_H
#define NOVDB_PLANNER_H

#include "parser.h"

/*
 * plan_stmt() — analyse and optimise an AST statement.
 *
 * Currently returns the ASTStmt* unchanged. When a plan-tree type
 * is introduced the return type will become PlanNode*.
 */
void *plan_stmt(void *stmt);

#endif /* NOVDB_PLANNER_H */
