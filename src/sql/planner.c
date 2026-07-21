/*
 * planner.c — SQL query planner / optimizer
 *
 * The planner analyses an AST statement and produces an execution
 * plan (operator tree) that the executor walks. Currently the
 * executor does its own ad-hoc planning, so this module is a
 * placeholder reserved for a cost-based optimizer.
 *
 * Future work:
 *   - Statistics collection (histogram, NDV)
 *   - Index selection via selectivity estimates
 *   - Join ordering with dynamic programming
 *   - Expression push-down into storage predicates
 */

#include "planner.h"
#include "parser.h"
#include "../common/logging.h"

/*
 * plan_stmt() — produce an execution plan for the given statement.
 *
 * For now the planner is a no-op: it returns the statement unchanged
 * and lets the executor decide how to run it. When a real plan tree
 * type is introduced, this function will transform ASTStmt → PlanNode.
 */
void *plan_stmt(void *stmt)
{
    /*
     * Pass the AST straight through. The executor treats a raw
     * ASTStmt* as a "trivial" plan node.
     */
    return stmt;
}
