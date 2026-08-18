/*-------------------------------------------------------------------------
 *
 * vfdw_search.c
 *		Running a nearest-neighbour search, and reading what comes back.
 *
 * The executor half of vfdw_knn.c, which decided at plan time that this query
 * is a search and what its k, its field and its operator's metric are. What
 * is left is everything that cannot be known until the query runs: the query
 * vector itself, and whether the index is the one the plan assumed.
 *
 * THE QUERY VECTOR IS NOT A PLAN-TIME VALUE. The spike measured a generic
 * plan carrying `ORDER BY emb <-> $1`, so the vector arrives with the
 * execution and not with the plan. FT.SEARCH is therefore built here, once
 * per pass, and a rescan builds it again rather than rewinding the reply -
 * the parameter may be a different vector, and replaying the old rows would
 * answer the new question with the old answer.
 *
 * ONE ROUND TRIP, TWO COMMANDS. FT.INFO and FT.SEARCH are pipelined together
 * because the check is not worth a second wait: FT.SEARCH is a read, so
 * issuing it before its verdict costs nothing but the work, and the verdict
 * is read first so a mismatch still raises before a single row is emitted.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_search.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_search.h"

#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"

#include "vfdw_cmd.h"
#include "vfdw_filter.h"
#include "vfdw_row.h"
#include "vfdw_scan_internal.h"
#include "vfdw_vec.h"

/*
 * The query vector, as bytes, from whatever expression the ORDER BY named.
 *
 * Through the expression's own OUTPUT function and then vfdw_vec_from_text,
 * which is the same route in reverse that a stored vector takes on the way
 * out - see vfdw_vec.c for why the text form is the join rather than any
 * binary one. Nothing here knows what a pgvector "vector" is, and that is the
 * point: any type whose text form is a vector literal works, and one whose is
 * not is refused naming itself.
 *
 * A NULL query vector is not a search. ORDER BY <col> <-> NULL asks for the
 * rows nearest to nothing, which has no answer, so it is refused rather than
 * turned into an empty result that would look like an empty table.
 */
static char *
vfdw_search_qvec(const VfdwKnnScan *knn, size_t *lenp)
{
	Datum		value;
	bool		isnull;
	Oid			typid = exprType((Node *) knn->qvec->expr);
	Oid			outfunc;
	bool		varlena;
	char	   *text;

	value = ExecEvalExpr(knn->qvec, knn->econtext, &isnull);

	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("the query vector is NULL"),
				 errdetail("A nearest-neighbour search ranks rows by their "
						   "distance from a point, and NULL is not one."),
				 errhint("Give ORDER BY a vector to search for.")));

	getTypeOutputInfo(typid, &outfunc, &varlena);
	text = OidOutputFunctionCall(outfunc, value);

	return vfdw_vec_from_text(text, "the query vector", lenp);
}

/*
 * FT.SEARCH <index> '*=>[KNN k @field $q AS alias]' PARAMS 2 q <bytes>
 *		DIALECT 2
 *
 * The vector travels as a PARAM rather than inside the query string, which is
 * not a style choice: it is binary, it contains NUL bytes by construction,
 * and a query string is parsed. PARAMS carries it as a length-counted
 * argument (invariant I3).
 *
 * No SORTBY. The spike found valkey-search rejects it outright - it diverges
 * from RediSearch here - and it would be redundant anyway, because KNN
 * results arrive already ranked. A design copied from RediSearch's
 * documentation would have sent a command this server refuses.
 */
static bool
vfdw_search_add_query(VfdwCmd *cmd, const VfdwKnnScan *knn,
					  const char *qvec, size_t qlen)
{
	StringInfoData q;

	initStringInfo(&q);

	/*
	 * The filter goes BEFORE the arrow, which is what makes it a pre-filter:
	 * the spike measured @tag:{b}=>[KNN 2 ...] returning one row out of three
	 * where two were asked for, so k applies to the filtered set. A `*` stands
	 * in when there is nothing to filter by.
	 */
	if (!vfdw_filter_render(knn->terms, knn->econtext, knn->bounds, &q))
	{
		pfree(q.data);
		return false;
	}

	if (q.len == 0)
		appendStringInfoChar(&q, '*');

	appendStringInfo(&q, "=>[KNN %d @%s $q AS %s]",
					 knn->k, knn->field, VFDW_SEARCH_DIST_ALIAS);

	vfdw_cmd_add_cstr(cmd, "FT.SEARCH");
	vfdw_cmd_add_cstr(cmd, knn->index);
	vfdw_cmd_add_cstr(cmd, q.data);
	vfdw_cmd_add_cstr(cmd, "PARAMS");
	vfdw_cmd_add_cstr(cmd, "2");
	vfdw_cmd_add_cstr(cmd, "q");
	vfdw_cmd_add_bytes(cmd, qvec, qlen);
	vfdw_cmd_add_cstr(cmd, "DIALECT");
	vfdw_cmd_add_cstr(cmd, "2");

	/*
	 * q.data is NOT freed here. VfdwCmd stores the pointer and the caller
	 * queues the command afterwards, so freeing it would send whatever the
	 * allocator left behind - which is how this first ran, and the server
	 * answered "Invalid filter format" about a string that was correct when
	 * it was built. The page context owns it and releases it with the pass.
	 */
	return true;
}

/*
 * A NULL bound: no row can satisfy the query, so nothing is asked.
 *
 * The FT.INFO already queued is still READ, for two reasons. A reply left in
 * the batch would be handed to whoever takes from it next, which is the class
 * of bug that gives one command another's answer. And it is worth reading on
 * its own merits: a table whose index disagrees with it should say so rather
 * than quietly return nothing, which is the same answer a correct empty filter
 * gives.
 */
static void
vfdw_search_unsatisfiable(VfdwScanState *state, size_t qlen)
{
	vfdw_search_verify(&state->knn, vfdw_batch_next(state->batch), qlen);

	state->knn.empty = true;
	state->knn.ran = true;
}

void
vfdw_search_run(VfdwScanState *state)
{
	VfdwKnnScan *knn = &state->knn;
	MemoryContext old;
	VfdwCmd		cmd;
	valkeyReply *info;
	char	   *qvec;
	size_t		qlen;

	if (knn->ran)
		return;

	/*
	 * In the page context, which a rescan resets: the command arguments and
	 * the converted vector are per-pass working memory, and the scan context
	 * would accumulate one copy of both per rescan.
	 */
	old = MemoryContextSwitchTo(state->page_cxt);

	qvec = vfdw_search_qvec(knn, &qlen);

	vfdw_cmd_init(&cmd, state->page_cxt, 10);
	vfdw_cmd_add_cstr(&cmd, "FT.INFO");
	vfdw_cmd_add_cstr(&cmd, knn->index);
	vfdw_batch_add(state->batch, &cmd);

	vfdw_cmd_reset(&cmd);
	if (!vfdw_search_add_query(&cmd, knn, qvec, qlen))
	{
		MemoryContextSwitchTo(old);
		vfdw_search_unsatisfiable(state, qlen);
		return;
	}
	vfdw_batch_add(state->batch, &cmd);

	MemoryContextSwitchTo(old);

	/*
	 * The verdict FIRST, so a query that disagrees with its index raises
	 * before a row is emitted - even though both commands were already in
	 * flight. Ordering the checks after the search would report the rows and
	 * the objection in the wrong order.
	 */
	info = vfdw_batch_next(state->batch);
	vfdw_search_verify(knn, info, qlen);

	knn->reply = vfdw_batch_next(state->batch);
	vfdw_reply_expect(knn->reply, VFDW_RTYPE(VALKEY_REPLY_ARRAY) |
					  VFDW_RTYPE(VALKEY_REPLY_MAP), "FT.SEARCH");

	knn->row = 0;
	knn->ran = true;
}

/*
 * Everything the plan settled about this search, copied into the scan's own
 * context.
 *
 * Copied rather than pointed at: fdw_private belongs to the plan, which a
 * cached generic plan outlives the execution of, and a scan that held
 * pointers into it would be reading a plan tree that a later invalidation may
 * have rebuilt.
 */
void
vfdw_search_adopt(VfdwScanState *state, ForeignScan *plan)
{
	MemoryContext old = MemoryContextSwitchTo(state->scan_cxt);

	state->knn.index = pstrdup(state->map->search_index);
	state->knn.field = pstrdup(vfdw_plan_knn_field(plan->fdw_private));
	state->knn.metric = pstrdup(vfdw_plan_knn_metric(plan->fdw_private));
	state->knn.k = vfdw_plan_knn_k(plan->fdw_private);
	state->knn.terms = vfdw_filter_decode(vfdw_plan_knn_filter(plan->fdw_private));

	MemoryContextSwitchTo(old);
}

/*
 * The query vector, as an expression rather than a value.
 *
 * Here rather than in vfdw_search_adopt because ExecInitExpr needs the
 * PlanState, and evaluated on every pass rather than now because a Param may
 * be a different vector each time.
 */
void
vfdw_search_init(VfdwScanState *state, ForeignScanState *node)
{
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;

	List	   *states = ExecInitExprList(plan->fdw_exprs, (PlanState *) node);

	/*
	 * Element zero is the query vector and the rest are the filter's bounds,
	 * in the order vfdw_filter_encode appended them. Paired by position, which
	 * is why one walk built both halves.
	 */
	state->knn.qvec = (ExprState *) linitial(states);
	state->knn.bounds = list_delete_first(states);
	state->knn.econtext = node->ss.ps.ps_ExprContext;
}

/*
 * The next row of a search, or an exhausted scan.
 *
 * FT.SEARCH answers [total, key, fields, key, fields, ...], so a row is two
 * elements and the count at element 0 is the number of documents that MATCHED
 * rather than the number returned. The rows are counted from the reply's own
 * element count instead: a server that matched a thousand documents still
 * sent k of them.
 *
 * INVARIANT I5. Running out of rows here is a real end - the search returned
 * everything it was going to - and it is reported by returning an empty slot,
 * which is this API's "scan finished". It is not the "no tuple this call"
 * that the keyspace loop uses when a key vanished, and the two must not be
 * spelled the same way. A search has no third state: it ran once, and what
 * came back is all there is.
 */
TupleTableSlot *
vfdw_search_next(VfdwScanState *state, TupleTableSlot *slot)
{
	VfdwKnnScan *knn = &state->knn;
	size_t		keyat;
	const valkeyReply *key;
	const valkeyReply *fields;

	CHECK_FOR_INTERRUPTS();

	vfdw_search_run(state);

	/* A NULL bound made the query unsatisfiable; nothing was asked. */
	if (knn->empty)
		return slot;

	keyat = (size_t) knn->row * 2 + 1;
	if (keyat + 1 >= knn->reply->elements)
		return slot;			/* exhausted, and genuinely so */

	key = vfdw_reply_child(knn->reply, keyat);
	fields = vfdw_reply_child(knn->reply, keyat + 1);
	knn->row++;

	/*
	 * A key with no bytes is not a document. Skipped rather than emitted as a
	 * row with a NULL key, and skipped by recursing rather than by looping,
	 * because the next row is the only thing left to try.
	 */
	if (key->str == NULL || fields->type != VALKEY_REPLY_ARRAY)
		return vfdw_search_next(state, slot);

	vfdw_scan_fill(&state->rowctx, slot, key->str, key->len,
				   (valkeyReply *) fields);
	return slot;
}

void
vfdw_search_reset(VfdwKnnScan *knn)
{
	knn->reply = NULL;
	knn->row = 0;
	knn->ran = false;
	knn->empty = false;
}
