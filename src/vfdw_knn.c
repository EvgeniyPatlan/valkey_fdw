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
#include "foreign/foreign.h"
#include "utils/lsyscache.h"

#include "vfdw_filter.h"
#include "vfdw_option.h"

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
	 * A join clause is still a refusal: it is applied above the scan by
	 * definition, and the rows it removes were already counted towards k. The
	 * relation's OWN clauses are a different matter - they are compiled into
	 * the search itself, or the query is refused - and that is decided by
	 * vfdw_filter_compile rather than here.
	 */
	if (baserel->joininfo != NIL)
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

/*
 * The relation's own restriction clauses, as bare expressions.
 *
 * From baserestrictinfo rather than from GetForeignPlan's scan_clauses,
 * because the matcher runs in GetForeignPaths too and there is no plan yet.
 * The two are the same clauses; only the wrapper differs.
 */
static List *
vfdw_knn_clauses(RelOptInfo *baserel)
{
	List	   *out = NIL;
	ListCell   *lc;

	foreach(lc, baserel->baserestrictinfo)
		out = lappend(out, ((RestrictInfo *) lfirst(lc))->clause);

	return out;
}

bool
vfdw_knn_match(PlannerInfo *root, RelOptInfo *baserel, VfdwTableMap *map,
			   VfdwKnnPlan *out)
{
	const char *why;

	if (map->tabletype != VFDW_TABLE_VECTOR)
		return false;
	if (!vfdw_knn_scan_owns_limit(root, baserel))
		return false;
	if (!vfdw_knn_match_order(root, baserel, map, out))
		return false;

	/*
	 * The filter last, because a query with no ORDER BY is not a search
	 * whatever its WHERE says. An empty clause list compiles to NIL terms,
	 * which is not a failure - NULL with a reason is.
	 */
	out->filter = vfdw_filter_compile(root, baserel, map,
									  vfdw_knn_clauses(baserel), &why);
	if (why != NULL)
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
	const char *why;

	if (root->query_pathkeys == NIL)
		return "the query has no ORDER BY, so there is no query vector to "
			"search for and no order to return rows in";

	if (!vfdw_knn_match_order(root, baserel, map, &scratch))
		return "the ORDER BY is not \"<vector column> <-> <expression>\" "
			"ascending, using a column that declares index_type 'vector'";

	if (baserel->joininfo != NIL)
		return "the query joins this table to another, so the LIMIT bounds "
			"the join's output rather than this scan's";

	(void) vfdw_filter_compile(root, baserel, map, vfdw_knn_clauses(baserel),
							   &why);
	if (why != NULL)
		return why;

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

/*
 * A search on a cluster is refused, and this is where.
 *
 * FT.SEARCH IS PER SHARD. Each node holds its own index over its own slots, so
 * a KNN query answered by one node returns that node's k nearest and calls
 * them the keyspace's. A correct top-K across shards means asking every
 * primary for k, then re-ranking the union and keeping k of it - and that
 * merge is what nothing here has.
 *
 * Phase 4's fan-out is not it. The fan-out visits every primary and
 * CONCATENATES what they return, which is exactly right for a keyspace scan -
 * a key belongs to one node, so the union is the keyspace - and exactly wrong
 * for a ranking, where the union of three ordered lists is not an ordered
 * list.
 *
 * Refused at plan time, and before the shape is examined: a query whose ORDER
 * BY is also wrong should be told about the cluster, because that is the fact
 * no rewrite of the query will change.
 *
 * The OPTION is read rather than the connection asked. There is no connection
 * at plan time and this must not open one - and the declaration is the right
 * thing to test anyway, because it is what routes every command this table
 * would send, whatever the server behind it turns out to be.
 */
void
vfdw_knn_require_standalone(Oid relid, VfdwTableMap *map)
{
	ForeignTable *table = GetForeignTable(relid);
	ForeignServer *server = GetForeignServer(table->serverid);
	VfdwServerOptions opts;

	vfdw_read_server_options(server->options, &opts);

	if (!opts.cluster)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot search a Valkey Cluster"),
			 errdetail("FT.SEARCH is answered by one node from its own slots, "
					   "so index \"%s\" on a cluster would return that node's "
					   "nearest rows as though they were the whole keyspace's. "
					   "A correct answer needs every primary asked and the "
					   "replies re-ranked, which is not implemented.",
					   map->search_index),
			 errhint("Point the table at a standalone server, or at one "
					 "cluster node declared without cluster 'true'.")));
}
