/*-------------------------------------------------------------------------
 *
 * vfdw_search.h
 *		Running a nearest-neighbour search, and reading what comes back.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_search.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_SEARCH_H
#define VFDW_SEARCH_H

#include "vfdw.h"

#include <valkey/valkey.h>

#include "foreign/fdwapi.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

#include "vfdw_conn.h"

/*
 * The alias the distance is returned under.
 *
 * A KNN clause has to name its score, and that name becomes a field of every
 * row alongside the hash's own. Deliberately not "dist" or "score": those are
 * names a user's hash could plausibly hold, and a collision would fill the
 * distance column with the document's own field and nobody would see it. This
 * one is not a name anyone would choose by accident, and if they did it is
 * their field that this wrapper would then read as a distance - so it is
 * checked, not merely improbable.
 */
#define VFDW_SEARCH_DIST_ALIAS "__vfdw_distance"

/*
 * How far over-fetching will go before it gives up.
 *
 * A filter that is not the query's qual can send the scan back for more rows,
 * and against a clause almost nothing satisfies that could walk the whole
 * index one doubling at a time. The cap turns that into an error naming the
 * clause rather than a query that appears to hang.
 */
#define VFDW_SEARCH_MAX_K	65536

/*
 * A search in progress.
 *
 * The reply is held across Iterate calls while its rows are emitted, which is
 * the lifetime the batch already gives a collection reply: the batch owns it
 * until the next take, and nothing else takes from the batch during a search.
 */
typedef struct VfdwKnnScan
{
	const char *index;
	const char *field;
	const char *metric;			/* what the OPERATOR means */
	int			k;

	ExprState  *qvec;			/* the query vector, evaluated when run */
	ExprContext *econtext;

	/*
	 * The WHERE clause, compiled at plan time and rendered per pass.
	 *
	 * terms and bounds are the same length and are paired by position: the
	 * terms came from fdw_private and the bounds from fdw_exprs, because a
	 * bound may be a Param and only fdw_exprs is renumbered.
	 */
	List	   *terms;
	List	   *bounds;

	/*
	 * A bound evaluated to NULL, so the comparison is NULL for every row and
	 * the query has no rows at all. Not the same state as an exhausted search
	 * and not spelled the same way, but it reaches the same end: no tuple,
	 * ever, and no command sent (invariant I5).
	 */
	bool		empty;

	valkeyReply *reply;
	int			row;			/* rows taken from the reply */
	bool		ran;

	/*
	 * OVER-FETCH, for a filter that is weaker than the query's own quals.
	 *
	 * A pushed filter that matches a SUPERSET is sound only if the scan can
	 * tell how many of the k rows it got actually satisfy the query - and
	 * fetch more when too few do. So the scan evaluates the quals itself and
	 * counts survivors: if k of them survive, those are the k nearest
	 * matching rows, because the ones it was given are the k nearest of the
	 * superset and anything not given is farther than all of them.
	 *
	 * ps is the ForeignScanState, for its compiled qual. The SAME ExprState
	 * the executor will apply above the scan, not a second compilation of the
	 * same clauses - two would be two chances to disagree, and the
	 * disagreement would be a row count.
	 */
	PlanState  *ps;
	int			emitted;		/* rows that survived the quals */
	int			fetched_k;		/* what the last FT.SEARCH asked for */
	int			returned;		/* rows the last reply carried */
	int			pass_survivors;	/* survivors seen in the current pass */
	bool		exact;			/* the filter is the qual, so no over-fetch */
} VfdwKnnScan;

struct VfdwScanState;

/*
 * Send the search, if it has not already been sent, and hand back the reply.
 *
 * The query vector is evaluated here rather than at plan time because it may
 * be a Param: a generic plan carries the expression and the parameter arrives
 * with the execution. That is also why a rescan re-runs the whole search
 * instead of rewinding the reply - the parameter may be a different vector.
 */
extern void vfdw_search_run(struct VfdwScanState *state);

/* Copy what the plan settled, and compile the query-vector expression. */
extern void vfdw_search_adopt(struct VfdwScanState *state, ForeignScan *plan);
extern void vfdw_search_init(struct VfdwScanState *state,
							 ForeignScanState *node);

/* The next row of the search, or an empty slot when it is exhausted. */
extern TupleTableSlot *vfdw_search_next(struct VfdwScanState *state,
										TupleTableSlot *slot);

/* Reset so the next fetch searches again. */
extern void vfdw_search_reset(VfdwKnnScan *knn);

/*
 * Verify the index against the query, or raise.
 *
 * Everything checked here is a difference that would otherwise produce
 * ordered, plausible, wrong rows: a field the index does not hold as a
 * vector, a metric that is not the one the operator asked for, a dimension
 * the query vector does not have, and an element type this wrapper does not
 * encode. The server is asked rather than the table trusted - the index is
 * the authority about itself, in the same way the slot map defers to it (I8).
 */
extern void vfdw_search_verify(const VfdwKnnScan *knn, valkeyReply *info,
							   size_t qbytes);

#endif							/* VFDW_SEARCH_H */
