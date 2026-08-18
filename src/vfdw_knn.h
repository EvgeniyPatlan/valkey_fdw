/*-------------------------------------------------------------------------
 *
 * vfdw_knn.h
 *		Recognising a nearest-neighbour query, and refusing everything else.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_knn.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_KNN_H
#define VFDW_KNN_H

#include "vfdw.h"

#include "nodes/pathnodes.h"

#include "vfdw_map.h"

/*
 * A matched nearest-neighbour query, as the planner found it.
 *
 * field is the valkey-search attribute the search ranks by, taken from the
 * column the ORDER BY names rather than from anything the user wrote twice.
 *
 * metric is what the OPERATOR means - L2 for <->, COSINE for <=>, IP for <#>.
 * FT.SEARCH is never told it: a KNN query names the field and the index
 * already knows how it measures distance. It travels anyway because the
 * executor checks it against what FT.INFO says the index actually uses, and a
 * disagreement is the one failure in this path that would otherwise produce
 * ordered, plausible, wrong rows.
 *
 * qvec is evaluated when the scan runs, never at plan time. It may be a
 * Param - the spike measured a generic plan doing exactly that - so there is
 * no query vector to build a command from until execution.
 */
typedef struct VfdwKnnPlan
{
	const char *field;
	const char *metric;
	Expr	   *qvec;
	int			k;

	/*
	 * The WHERE clause, compiled into the search's own filter language. NIL
	 * when the query has no restriction clauses at all.
	 *
	 * A List of VfdwFilterTerm; see src/vfdw_filter.h for why a clause that
	 * cannot be compiled refuses the whole query rather than being left to the
	 * executor.
	 */
	List	   *filter;
} VfdwKnnPlan;

/*
 * Does this query have the one shape a vector table can answer?
 *
 * Returns false without raising, so the caller decides whether a non-match is
 * a refusal (a vector table) or simply nothing to do (every other table).
 */
extern bool vfdw_knn_match(PlannerInfo *root, RelOptInfo *baserel,
						   VfdwTableMap *map, VfdwKnnPlan *out);

/*
 * Refuse a query against a vector table, naming the first thing about it that
 * is not the shape a search can serve.
 *
 * Separate from the matcher because a bool cannot say WHY, and "this query is
 * not supported" leaves a user to guess between the join, the WHERE and the
 * missing LIMIT. Re-walks the query for the message rather than threading a
 * reason out of the matcher: this runs once, on the way to an error.
 */
extern void vfdw_knn_refuse(PlannerInfo *root, RelOptInfo *baserel,
							VfdwTableMap *map);

/*
 * Refuse a search on a server declared cluster 'true'.
 *
 * Called before the shape is examined, so a query that is wrong in two ways is
 * told about the one no rewrite of it will fix. See the definition for why the
 * fan-out phase 4 already has is not the merge this needs.
 */
extern void vfdw_knn_require_standalone(Oid relid, VfdwTableMap *map);

#endif							/* VFDW_KNN_H */
