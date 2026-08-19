/*-------------------------------------------------------------------------
 *
 * vfdw_plan_param.c
 *		The join clause this table can be driven by, one key at a time.
 *
 * Split from src/vfdw_plan.c, which chooses among the paths a table can offer
 * on its own. This file answers a different question - what ANOTHER relation
 * can ask this one for - and it reaches into the planner's equivalence
 * classes to do it, which nothing else here does.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_plan_param.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_scan.h"

#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"

#include "vfdw_plan_internal.h"

/*
 * Does this equivalence-class member name THIS table's key column?
 *
 * The callback generate_implied_equalities_for_column asks per member. It is
 * how the key Var is recognised inside an EC, which is where the interesting
 * clauses actually live - see vfdw_plan_param_key.
 */
static bool
vfdw_param_key_member(PlannerInfo *root, RelOptInfo *rel,
					  EquivalenceClass *ec, EquivalenceMember *em, void *arg)
{
	(void) root;
	(void) ec;

	return vfdw_is_key_var(rel, (VfdwTableMap *) arg, (Node *) em->em_expr);
}

/*
 * A join clause of the form "key = <something from another relation>".
 *
 * WHAT THIS IS FOR. Without it a join against this table has exactly one path
 * on offer - walk the keyspace - so the join clause can only ever be a filter
 * ABOVE the scan, and answering three rows costs a walk of every key. With it
 * the planner may put this relation on the inner side of a nested loop and ask
 * for one key per outer row, which is one GET instead of a full scan.
 *
 * Measured before it existed, against 2500 keys: a three-row join sent 2503
 * commands. It is the same shape postgres_fdw's parameterised paths serve, and
 * the same reason.
 *
 * NOT FROM joininfo, which is where the first version of this looked and
 * found nothing. `t.key = o.key` is mergejoinable, so the planner does not
 * leave it in joininfo at all - it folds it into an EquivalenceClass, and the
 * clause has to be asked for with generate_implied_equalities_for_column.
 * joininfo holds what is left over, which for this shape is nothing.
 *
 * The expression is NOT evaluated here and cannot be: it names a column of a
 * relation this scan knows nothing about. What is decided here is only that it
 * is a key equality and which relations it needs; the value arrives per rescan.
 */
Node *
vfdw_plan_param_key(PlannerInfo *root, RelOptInfo *baserel, VfdwTableMap *map,
					Relids *outer)
{
	List	   *clauses;
	ListCell   *lc;

	if (!vfdw_key_column_pushable(map))
		return NULL;

	clauses = generate_implied_equalities_for_column(root, baserel,
													vfdw_param_key_member,
													map, NULL);

	foreach(lc, clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		OpExpr	   *op;
		Node	   *left;
		Node	   *right;

		if (!IsA(rinfo->clause, OpExpr))
			continue;

		op = (OpExpr *) rinfo->clause;
		if (list_length(op->args) != 2)
			continue;

		left = (Node *) linitial(op->args);
		right = (Node *) lsecond(op->args);

		if (!vfdw_is_key_var(baserel, map, left))
		{
			Node	   *tmp = left;

			left = right;
			right = tmp;
		}
		if (!vfdw_is_key_var(baserel, map, left))
			continue;
		if (!vfdw_is_equality(op->opno, ((Var *) left)->vartype))
			continue;

		*outer = bms_difference(rinfo->clause_relids, baserel->relids);
		if (bms_is_empty(*outer))
			continue;

		return right;
	}

	return NULL;
}

