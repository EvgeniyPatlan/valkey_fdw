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
#include <limits.h>

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
					 knn->fetched_k, knn->field, VFDW_SEARCH_DIST_ALIAS);

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

/*
 * Queue FT.INFO and FT.SEARCH, or settle that there is nothing to ask.
 *
 * Returns false when a bound evaluated to NULL: the query has no rows and
 * vfdw_search_unsatisfiable has already read the FT.INFO reply, so the caller
 * has nothing left to do.
 */
static bool
vfdw_search_send(VfdwScanState *state, size_t *qlen)
{
	VfdwKnnScan *knn = &state->knn;
	MemoryContext old = MemoryContextSwitchTo(state->page_cxt);
	VfdwCmd		cmd;
	char	   *qvec;

	qvec = vfdw_search_qvec(knn, qlen);

	vfdw_cmd_init(&cmd, state->page_cxt, 10);
	vfdw_cmd_add_cstr(&cmd, "FT.INFO");
	vfdw_cmd_add_cstr(&cmd, knn->index);
	vfdw_batch_add(state->batch, &cmd);

	vfdw_cmd_reset(&cmd);
	if (!vfdw_search_add_query(&cmd, knn, qvec, *qlen))
	{
		MemoryContextSwitchTo(old);
		vfdw_search_unsatisfiable(state, *qlen);
		return false;
	}
	vfdw_batch_add(state->batch, &cmd);

	MemoryContextSwitchTo(old);
	return true;
}

void
vfdw_search_run(VfdwScanState *state)
{
	VfdwKnnScan *knn = &state->knn;
	valkeyReply *info;
	size_t		qlen;

	if (knn->ran)
		return;

	/* The first pass asks for exactly k; a re-issue asks for more. */
	if (knn->fetched_k == 0)
		knn->fetched_k = knn->k;

	/*
	 * In the page context, which a rescan resets: the command arguments and
	 * the converted vector are per-pass working memory, and the scan context
	 * would accumulate one copy of both per rescan.
	 */
	if (!vfdw_search_send(state, &qlen))
		return;

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

	/*
	 * How many rows came back. The ceiling on over-fetching, and it has to be
	 * this rather than element 0 of the reply: that element is the number
	 * RETURNED, bounded by k, not the number the filter matched. Reading it as
	 * a match total made every over-fetch decide it had already seen
	 * everything, which is the one answer that looks like success.
	 *
	 * A reply SHORTER than what was asked for is exhaustion, definitively: the
	 * filter has no more documents to give. That costs nothing to observe,
	 * where asking the server for a match total would be another round trip.
	 */
	knn->returned = (int) ((knn->reply->elements - 1) / 2);
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

	/*
	 * Whether the filter IS the qual. When it is not - a tag term matches a
	 * superset - the scan must recheck and fetch more when too few rows
	 * survive, which is what vfdw_search_grow does.
	 */
	state->knn.exact = vfdw_filter_is_exact(state->knn.terms);

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
	state->knn.ps = (PlanState *) node;
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
/*
 * Does this row satisfy the query's own quals?
 *
 * The SAME ExprState the executor applies above the scan, taken from the
 * PlanState rather than compiled again here. Two compilations of one clause
 * list are two chances to disagree, and here the disagreement would decide how
 * many rows to fetch - so the scan would over-fetch against one rule and the
 * executor would filter by another.
 *
 * True when there are no quals at all, which is the ordinary case and needs no
 * special path: nothing to fail.
 */
static bool
vfdw_search_row_wanted(VfdwKnnScan *knn, TupleTableSlot *slot)
{
	ExprContext *econtext;

	if (knn->ps == NULL || knn->ps->qual == NULL)
		return true;

	econtext = knn->ps->ps_ExprContext;
	econtext->ecxt_scantuple = slot;

	return ExecQual(knn->ps->qual, econtext);
}

/*
 * Ask again, for more rows.
 *
 * Only when the last answer had too few SURVIVORS and the filter matched more
 * documents than were asked for. Doubling rather than incrementing, because
 * the cost of another pass is a round trip and the cost of guessing low is
 * another one after it.
 *
 * Bounded by the match count the reply carries: once k reaches it, every
 * document the filter matched has been seen and there is nothing left to ask
 * for. That is what makes this terminate rather than grow forever against a
 * qual nothing satisfies.
 */
static bool
vfdw_search_grow(VfdwScanState *state)
{
	VfdwKnnScan *knn = &state->knn;

	if (knn->exact || knn->emitted >= knn->k)
		return false;

	/*
	 * The last pass got fewer rows than it asked for, so the filter has
	 * nothing else to give and asking again would return the same rows.
	 */
	if (knn->returned < knn->fetched_k)
		return false;

	if (knn->fetched_k >= VFDW_SEARCH_MAX_K)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("gave up over-fetching a vector search after %d rows",
						knn->fetched_k),
				 errdetail("The WHERE clause is not expressible as a search "
						   "filter, so the search is asked for more rows than "
						   "the query wants and the surplus is discarded. Too "
						   "few of them satisfied the clause."),
				 errhint("Narrow the query with a condition the index can "
						 "apply - a numeric range, or a tag whose values hold "
						 "no separator - or raise the LIMIT.")));

	knn->fetched_k = knn->fetched_k < VFDW_SEARCH_MAX_K / 2
		? knn->fetched_k * 2 : VFDW_SEARCH_MAX_K;

	/*
	 * emitted is NOT reset. The re-issued search returns the same ranking
	 * with more of it, so its first `emitted` survivors are the rows already
	 * given to the executor - and emitting them again is a duplicate row,
	 * which is what the first version of this did: LIMIT 2 answered the same
	 * key twice. pass_survivors counts this pass's survivors so they can be
	 * skipped up to that mark.
	 */
	knn->ran = false;
	knn->row = 0;
	knn->pass_survivors = 0;
	MemoryContextReset(state->page_cxt);
	vfdw_search_run(state);
	return true;
}

/*
 * What became of one row of the reply.
 *
 * DONE and EMIT both hand the slot back; they differ in whether it holds a
 * tuple, and keeping them apart is what stops "k reached" from reading as
 * "another row" (invariant I5).
 */
typedef enum VfdwRowFate
{
	VFDW_ROW_SKIP,				/* not this one; keep reading */
	VFDW_ROW_EMIT,				/* a row for the executor */
	VFDW_ROW_DONE				/* k reached; the slot is empty */
} VfdwRowFate;

static VfdwRowFate
vfdw_search_take_row(VfdwScanState *state, TupleTableSlot *slot,
					 const valkeyReply *key, const valkeyReply *fields)
{
	VfdwKnnScan *knn = &state->knn;

	/* A key with no bytes is not a document. */
	if (key->str == NULL || fields->type != VALKEY_REPLY_ARRAY)
		return VFDW_ROW_SKIP;

	vfdw_scan_fill(&state->rowctx, slot, key->str, key->len,
				   (valkeyReply *) fields);

	/*
	 * ROWS THE QUALS REJECT ARE NOT EMITTED, and not merely left for the
	 * executor. The count of survivors is what decides whether to ask for
	 * more, so the scan has to know it - and a row emitted and then filtered
	 * above would be counted here as a survivor.
	 */
	if (!vfdw_search_row_wanted(knn, slot))
	{
		ExecClearTuple(slot);
		return VFDW_ROW_SKIP;
	}

	knn->pass_survivors++;

	/*
	 * Already given to the executor by an earlier pass. The re-issued search
	 * repeats the ranking it had, so its leading survivors are the rows
	 * already emitted - and emitting them again is a duplicate row, which is
	 * what the first version of this loop did: LIMIT 2 answered one key twice.
	 */
	if (knn->pass_survivors <= knn->emitted)
	{
		ExecClearTuple(slot);
		return VFDW_ROW_SKIP;
	}

	/*
	 * And no more than k. The server was asked for more than k on a re-issue,
	 * so the surplus survivors are real rows that a LIMIT above would discard
	 * anyway - but emitting them would make a query whose own LIMIT is k
	 * return more than k.
	 */
	if (knn->emitted >= knn->k)
	{
		ExecClearTuple(slot);
		return VFDW_ROW_DONE;
	}

	knn->emitted++;
	return VFDW_ROW_EMIT;
}

TupleTableSlot *
vfdw_search_next(VfdwScanState *state, TupleTableSlot *slot)
{
	VfdwKnnScan *knn = &state->knn;

	CHECK_FOR_INTERRUPTS();

	vfdw_search_run(state);

	/* A NULL bound made the query unsatisfiable; nothing was asked. */
	if (knn->empty)
		return slot;

	for (;;)
	{
		size_t		keyat = (size_t) knn->row * 2 + 1;
		const valkeyReply *key;
		const valkeyReply *fields;

		CHECK_FOR_INTERRUPTS();

		if (keyat + 1 >= knn->reply->elements)
		{
			/*
			 * The reply is spent. If the quals took away more rows than the
			 * over-fetch allowed for, ask again for more; otherwise this is a
			 * real end (invariant I5).
			 */
			if (vfdw_search_grow(state))
				continue;
			return slot;
		}

		key = vfdw_reply_child(knn->reply, keyat);
		fields = vfdw_reply_child(knn->reply, keyat + 1);
		knn->row++;

		switch (vfdw_search_take_row(state, slot, key, fields))
		{
			case VFDW_ROW_SKIP:
				continue;
			case VFDW_ROW_EMIT:
			case VFDW_ROW_DONE:
				return slot;
		}
	}
}

void
vfdw_search_reset(VfdwKnnScan *knn)
{
	knn->reply = NULL;
	knn->row = 0;
	knn->ran = false;
	knn->empty = false;
	knn->emitted = 0;
	knn->fetched_k = 0;
	knn->returned = 0;
	knn->pass_survivors = 0;
}
