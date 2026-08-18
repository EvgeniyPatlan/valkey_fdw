/*-------------------------------------------------------------------------
 *
 * vfdw_knn.c
 *		Recognising a nearest-neighbour query, and refusing everything else.
 *
 * A top-K query is not a scan with a filter. FT.SEARCH ... KNN k is a
 * RANKING, and a ranking only means anything for a given k and a given query
 * vector - so unlike every other narrowing this wrapper does, getting this
 * wrong is a wrong answer rather than a slow one. A scan that is asked for
 * ten nearest and returns ten rows in some other order looks exactly like a
 * correct one.
 *
 * That is why this file recognises ONE shape and refuses the rest:
 *
 *		SELECT ... FROM <vector table> ORDER BY <col> <op> <expr> LIMIT <const>
 *
 * with no join, no WHERE, no grouping, and nothing else in the FROM list.
 * Each of those exclusions is a way k stops being the number of rows the
 * server should be asked for:
 *
 *	- A JOIN or a WHERE takes rows away AFTER the search. The true k nearest
 *	  MATCHING rows are not the matching subset of the k nearest, and the rows
 *	  needed to tell the difference were never fetched. The spike proved this
 *	  empirically: valkey-search applies a pre-filter BEFORE the KNN, and
 *	  @tag:{b}=>[KNN 2 ...] over three rows of which one is tagged b returned
 *	  one row, not two. Pushing the filter down is 6.4; until then a WHERE is
 *	  refused rather than applied locally.
 *	- GROUP BY, DISTINCT and friends consume more rows than they emit, so the
 *	  LIMIT above them says nothing about how many the scan must produce.
 *
 * RECOGNITION IS BY OPERATOR NAME, not by OID. The spike measured pgvector's
 * <-> at OID 16432 and its vector type at 16385, both assigned by CREATE
 * EXTENSION and therefore different in every other database. Nothing here is
 * linked against pgvector or requires it to be installed; what is required is
 * an operator of one of three names taking a value of this table's vector
 * column and returning float8.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_knn.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_knn.h"

#include <limits.h>

#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"

/*
 * PostgreSQL 18 replaced PathKey.pk_strategy with pk_cmptype. Both spell the
 * same two directions; only the name and the constant changed.
 */
#if PG_VERSION_NUM >= 180000
#define VFDW_PK_IS_ASC(pk)	((pk)->pk_cmptype == COMPARE_LT)
#else
#define VFDW_PK_IS_ASC(pk)	((pk)->pk_strategy == BTLessStrategyNumber)
#endif

/*
 * The three distance operators, and what each one measures.
 *
 * pgvector's names, because they are the ones a user will have written and
 * the ones an index was almost certainly created to match. The metric is not
 * sent to the server - a KNN query names a field, and the index knows its own
 * metric - it is carried so the executor can compare the two and refuse a
 * query whose operator disagrees with the index it would run against.
 */
static const struct
{
	const char *opname;
	const char *metric;
}			vfdw_knn_operators[] =
{
	{"<->", "L2"},
	{"<=>", "COSINE"},
	{"<#>", "IP"},
};

static const char *
vfdw_knn_metric_for(Oid opno)
{
	char	   *name = get_opname(opno);
	size_t		i;

	if (name == NULL)
		return NULL;

	for (i = 0; i < lengthof(vfdw_knn_operators); i++)
	{
		if (strcmp(name, vfdw_knn_operators[i].opname) == 0)
		{
			pfree(name);
			return vfdw_knn_operators[i].metric;
		}
	}

	pfree(name);
	return NULL;
}

/*
 * The vector field this Var names, or NULL.
 *
 * The attnum is checked against the tuple descriptor's width before it is
 * used as an index, for the reason vfdw_plan.c's key check gives: a whole-row
 * or system Var has an attnum that is zero or negative, and indexing with it
 * reads outside the array.
 *
 * index_type 'vector' is what distinguishes the embedding from every other
 * field of the same hash. Without it a tag column of a text type would match
 * as readily, and the search would rank by a field the index does not hold as
 * a vector at all.
 */
static const char *
vfdw_knn_vector_field(Node *node, RelOptInfo *baserel, VfdwTableMap *map)
{
	Var		   *var;
	VfdwColumn *col;

	node = (Node *) strip_implicit_coercions((Node *) node);

	if (node == NULL || !IsA(node, Var))
		return NULL;

	var = (Var *) node;
	if (var->varno != (int) baserel->relid || var->varlevelsup != 0)
		return NULL;
	if (var->varattno < 1 || var->varattno > map->natts)
		return NULL;

	col = &map->cols[var->varattno - 1];
	if (col->kind != VFDW_COL_FIELD || col->index_type != VFDW_INDEX_VECTOR)
		return NULL;

	return col->field;
}

/*
 * Is this OpExpr "<vector column> <distance op> <something else>"?
 *
 * Accepts the column on EITHER side, because the spike measured the planner
 * keeping both spellings: `emb <-> const` and `const <-> emb` produce the
 * same pathkey with the operands in the order they were written. Distance is
 * symmetric for all three metrics, so which side the column is on changes
 * nothing about the search.
 *
 * The other side must not mention this relation. A row-dependent query vector
 * would be a different search per row, which FT.SEARCH cannot express and
 * which nothing here would notice: it would build one command from whatever
 * the expression evaluated to once.
 */
static bool
vfdw_knn_match_opexpr(PlannerInfo *root, RelOptInfo *baserel,
					  VfdwTableMap *map, OpExpr *op, VfdwKnnPlan *out)
{
	const char *metric;
	const char *field;
	Node	   *other;

	if (list_length(op->args) != 2 || op->opresulttype != FLOAT8OID)
		return false;

	metric = vfdw_knn_metric_for(op->opno);
	if (metric == NULL)
		return false;

	field = vfdw_knn_vector_field(linitial(op->args), baserel, map);
	other = (Node *) lsecond(op->args);

	if (field == NULL)
	{
		field = vfdw_knn_vector_field(lsecond(op->args), baserel, map);
		other = (Node *) linitial(op->args);
	}

	if (field == NULL)
		return false;

	if (bms_is_member(baserel->relid, pull_varnos(root, other)))
		return false;

	out->field = field;
	out->metric = metric;
	out->qvec = (Expr *) other;
	return true;
}

/*
 * The ordering, if it is one this scan can produce.
 *
 * One pathkey exactly. A second would be a tie-break the server was never
 * told about, and rows that happen to arrive in the right order for it are
 * not the same thing as rows sorted by it.
 *
 * Ascending only, and NULLs last, because that is what a nearest-neighbour
 * search returns: nearest first. DESC would be the FARTHEST k, which no
 * amount of reversing a k-row result answers - the rows needed are the ones
 * the search did not return.
 */
static bool
vfdw_knn_match_order(PlannerInfo *root, RelOptInfo *baserel,
					 VfdwTableMap *map, VfdwKnnPlan *out)
{
	PathKey    *pk;
	ListCell   *lc;

	if (list_length(root->query_pathkeys) != 1)
		return false;

	pk = (PathKey *) linitial(root->query_pathkeys);
	if (!VFDW_PK_IS_ASC(pk) || pk->pk_nulls_first)
		return false;

	foreach(lc, pk->pk_eclass->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);
		Node	   *expr = (Node *) strip_implicit_coercions((Node *) em->em_expr);

		if (expr != NULL && IsA(expr, OpExpr) &&
			vfdw_knn_match_opexpr(root, baserel, map, (OpExpr *) expr, out))
			return true;
	}

	return false;
}

/*
 * Is the LIMIT this scan's own, and a number?
 *
 * root->limit_tuples is count + offset, or -1 when either is not a plan-time
 * constant - which is exactly the "refuse a parameterised LIMIT" rule, got
 * from the planner rather than re-derived here.
 *
 * It is the number of rows THE QUERY needs, so it is only the number of rows
 * THIS SCAN needs when nothing between the two changes the count. Hence the
 * checks either side of it: one base relation, and no operator above the scan
 * that consumes more rows than it emits. Reading limit_tuples without them is
 * the mistake that makes a join return the k nearest of the wrong set.
 */
static bool
vfdw_knn_scan_owns_limit(PlannerInfo *root, RelOptInfo *baserel)
{
	Query	   *parse = root->parse;
	int			rti;
	int			nbase = 0;

	if (parse->commandType != CMD_SELECT)
		return false;
	if (parse->groupClause != NIL || parse->groupingSets != NIL ||
		parse->distinctClause != NIL || parse->havingQual != NULL ||
		parse->hasWindowFuncs || parse->hasAggs || parse->setOperations != NULL)
		return false;

	/*
	 * Every restriction clause on this relation is applied above the scan and
	 * would take rows out of the k the server returned. 6.4 pushes what it
	 * can into the query language; until then having any is a refusal.
	 */
	if (baserel->baserestrictinfo != NIL || baserel->joininfo != NIL)
		return false;

	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];

		if (rel != NULL && rel->reloptkind == RELOPT_BASEREL)
			nbase++;
	}

	if (nbase != 1)
		return false;

	return root->limit_tuples >= 1 && root->limit_tuples <= (double) INT_MAX;
}

bool
vfdw_knn_match(PlannerInfo *root, RelOptInfo *baserel, VfdwTableMap *map,
			   VfdwKnnPlan *out)
{
	if (map->tabletype != VFDW_TABLE_VECTOR)
		return false;
	if (!vfdw_knn_scan_owns_limit(root, baserel))
		return false;
	if (!vfdw_knn_match_order(root, baserel, map, out))
		return false;

	out->k = (int) root->limit_tuples;
	return true;
}

/*
 * Which part of the shape is missing, as a sentence.
 *
 * Ordered so the first thing a user has to change is the first thing named,
 * rather than by how cheap the test is: someone who wrote no ORDER BY at all
 * is not helped by being told about the LIMIT first.
 */
static const char *
vfdw_knn_missing(PlannerInfo *root, RelOptInfo *baserel, VfdwTableMap *map)
{
	VfdwKnnPlan scratch;

	if (root->query_pathkeys == NIL)
		return "the query has no ORDER BY, so there is no query vector to "
			"search for and no order to return rows in";

	if (!vfdw_knn_match_order(root, baserel, map, &scratch))
		return "the ORDER BY is not \"<vector column> <-> <expression>\" "
			"ascending, using a column that declares index_type 'vector'";

	if (baserel->baserestrictinfo != NIL || baserel->joininfo != NIL)
		return "the query has a WHERE clause, which would remove rows the "
			"search already counted towards k";

	if (root->parse->commandType != CMD_SELECT)
		return "only SELECT reads a vector table";

	if (root->limit_tuples < 1)
		return "the query has no LIMIT, or its LIMIT is not a constant, so "
			"there is no k to search for";

	return "the LIMIT does not bound this scan alone - a join, a grouping or "
		"a second table stands between it and the rows the search returns";
}

void
vfdw_knn_refuse(PlannerInfo *root, RelOptInfo *baserel, VfdwTableMap *map)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("this query cannot be answered by a vector search"),
			 errdetail("A tabletype 'vector' table is read only by "
					   "FT.SEARCH against index \"%s\", and %s.",
					   map->search_index,
					   vfdw_knn_missing(root, baserel, map)),
			 errhint("The shape that runs is: SELECT ... FROM t ORDER BY "
					 "<vector column> <-> <query vector> LIMIT <constant>, "
					 "with no WHERE and no join.")));
}
