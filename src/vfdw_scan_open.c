/*-------------------------------------------------------------------------
 *
 * vfdw_scan_open.c
 *		A scan's life: opening it, telling it what the plan decided, and
 *		starting it over.
 *
 * Split from src/vfdw_scan.c, which is the producer - the page loop, the
 * reply shapes and the row it builds from them. This file is everything
 * either side of that loop, and the two grew apart rather than together: the
 * producer changes when Valkey's replies do, and this changes when the PLAN
 * gains something to say. Vector search added to both at once, and the file
 * that held them together outgrew the length gate.
 *
 * The two halves meet at VfdwScanState, which src/vfdw_scan_internal.h
 * defines for exactly this reason.
 *
 * WHAT LIVES HERE AND NOT IN BeginForeignScan: ANALYZE has no executor node
 * and no plan, and drives vfdw_scan_state_create directly so that its sample
 * comes from the same producer a query reads through. So the constructor
 * takes a Relation and a context, and everything that needs a ForeignScan is
 * layered on top of it rather than built into it.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_scan_open.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_scan.h"

#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "vfdw_cluster.h"
#include "vfdw_cmd.h"
#include "vfdw_map.h"
#include "vfdw_row.h"
#include "vfdw_scan_cluster.h"
#include "vfdw_scan_internal.h"
#include "vfdw_scan_overlay.h"
#include "vfdw_search.h"
#include "vfdw_ttl.h"

/* See vfdw_scan_batch_stats in vfdw_scan.h for what these are for. */
static uint64 vfdw_scan_batch_contexts = 0;
static uint64 vfdw_scan_batch_resets = 0;

void
vfdw_scan_batch_stats(uint64 *contexts, uint64 *resets)
{
	*contexts = vfdw_scan_batch_contexts;
	*resets = vfdw_scan_batch_resets;
}

/*
 * Record the keys a scan will fetch, in the scan's own context so they
 * outlive the per-page reset and survive a rescan.
 */
static void
vfdw_scan_set_keys(VfdwScanState *state, List *keys)
{
	MemoryContext old = MemoryContextSwitchTo(state->scan_cxt);
	ListCell   *lc;
	int			i = 0;

	state->n_plan_keys = list_length(keys);
	state->plan_keys = palloc(sizeof(char *) * Max(state->n_plan_keys, 1));
	state->plan_keylens = palloc(sizeof(size_t) * Max(state->n_plan_keys, 1));

	foreach(lc, keys)
	{
		const char *key = strVal((String *) lfirst(lc));

		state->plan_keys[i] = pstrdup(key);
		state->plan_keylens[i] = strlen(key);
		i++;
	}

	MemoryContextSwitchTo(old);
}

/*
 * Take the access path the planner chose.
 *
 * The key travels down with it when there is exactly one. Restriction clauses
 * stay in the plan either way, so this is purely an access-path choice:
 * getting it wrong could only cost a round trip, never return a row the query
 * did not ask for.
 */
static void
vfdw_scan_adopt_plan(VfdwScanState *state, ForeignScan *plan)
{
	const char *pattern;

	state->strategy = (VfdwScanStrategy)
		intVal(list_nth(plan->fdw_private, VFDW_PRIV_STRATEGY));

	if (state->strategy == VFDW_SCAN_KNN)
	{
		vfdw_search_adopt(state, plan);
		return;
	}

	if (state->strategy != VFDW_SCAN_KEYSPACE)
		vfdw_scan_set_keys(state, vfdw_plan_keys(plan->fdw_private));

	pattern = vfdw_plan_pattern(plan->fdw_private);
	state->pattern = pattern != NULL
		? MemoryContextStrdup(state->scan_cxt, pattern) : NULL;
}

/*
 * Everything a scan needs except the access path.
 *
 * Shared by the executor and by ANALYZE so that the sample is drawn by the
 * same producer a query uses. A separate sampling path would be free to
 * disagree with the scan about what the table contains, and statistics that
 * describe a different table than the one being read are worse than none.
 */
VfdwScanState *
vfdw_scan_state_create(Relation rel, MemoryContext parent)
{
	ForeignTable *table = GetForeignTable(RelationGetRelid(rel));
	ForeignServer *server = GetForeignServer(table->serverid);
	UserMapping *user = GetUserMapping(GetUserId(), table->serverid);
	MemoryContext scan_cxt = AllocSetContextCreate(parent, "valkey_fdw scan",
												   ALLOCSET_DEFAULT_SIZES);
	VfdwScanState *state;

	state = MemoryContextAllocZero(scan_cxt, sizeof(VfdwScanState));
	state->scan_cxt = scan_cxt;
	state->page_cxt = AllocSetContextCreate(scan_cxt, "valkey_fdw scan page",
											ALLOCSET_SMALL_SIZES);
	state->map = vfdw_map_build(rel, table);
	state->serverid = server->serverid;

	/*
	 * Cluster-aware from here on: this scan fans out across primaries, so it
	 * is entitled to the connection a plain caller is still refused.
	 */
	state->vconn = vfdw_get_connection_cluster(server, user);

	vfdw_conn_select_db(state->vconn, state->map->database);

	/*
	 * Before a single command is sent, so a server that cannot answer for
	 * expiry says so instead of answering the first page and then failing.
	 * This is the earliest point there is a server to ask: the map is resolved
	 * at plan time, where there is no connection and so no question to put.
	 */
	if (state->map->nttl > 0)
		vfdw_ttl_require(state->vconn);

	vfdw_row_ctx_init(&state->rowctx, state->map, scan_cxt);

	state->cur_hlen = -1;
	state->cur_member = -1;

	vfdw_scan_batch_contexts++;
	state->batch_cxt = AllocSetContextCreate(scan_cxt, "valkey_fdw scan batch",
											 ALLOCSET_SMALL_SIZES);
	state->batch = vfdw_batch_begin(state->vconn, state->batch_cxt);
	vfdw_scan_cluster_setup(state, server, user);

	/*
	 * What the table alone determines. A plan, when there is one, narrows
	 * this further; ANALYZE has no plan and reads the table as defined.
	 */
	state->pattern = vfdw_plan_scan_pattern(state->map, NULL);

	if (state->map->singleton_key != NULL)
	{
		state->strategy = VFDW_SCAN_SINGLETON;
		vfdw_scan_set_keys(state,
						   list_make1(makeString(pstrdup(state->map->singleton_key))));
	}

	return state;
}

void
vfdw_scan_begin(ForeignScanState *node, int eflags)
{
	EState	   *estate = node->ss.ps.state;
	Relation	rel = node->ss.ss_currentRelation;
	VfdwScanState *state;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	state = vfdw_scan_state_create(rel, estate->es_query_cxt);
	vfdw_scan_adopt_plan(state, (ForeignScan *) node->ss.ps.plan);

	if (state->strategy == VFDW_SCAN_KNN)
		vfdw_search_init(state, node);

	/*
	 * The snapshot's curcid, captured once per scan. An entry is visible only
	 * if it is OLDER than this, which is PostgreSQL's own heap rule and is
	 * what stops INSERT INTO t SELECT * FROM t re-reading its own output:
	 * that is one command, so its writes carry this very cid and stay
	 * invisible. INSERT then SELECT is two, and the second sees the first.
	 */
	state->overlay_cid = estate->es_snapshot->curcid;

	node->fdw_state = state;
}


void
vfdw_scan_rescan(ForeignScanState *node)
{
	VfdwScanState *state = (VfdwScanState *) node->fdw_state;

	if (state == NULL)
		return;

	/*
	 * Restart from the beginning, not from wherever the previous pass
	 * stopped. Resetting only the row counter makes a rescan replay the last
	 * page that happened to be held - or, with a pushed-down qual, produce
	 * nothing at all.
	 */
	vfdw_batch_end(state->batch);
	MemoryContextReset(state->page_cxt);

	if (state->cursor != NULL)
		pfree(state->cursor);
	state->cursor = NULL;
	state->started = false;
	state->keys = NULL;
	state->keylens = NULL;
	state->nkeys = 0;
	state->next_key = 0;

	/*
	 * The seen-set and the tail iterator restart with everything else. Not
	 * clearing them would make the second pass of a nested loop skip every
	 * injected row - the same silent truncation, arriving through the overlay
	 * instead of through the cursor.
	 */
	vfdw_scan_seen_reset(state);
	state->overlay_tail = false;
	state->cur_reply = NULL;
	state->cur_elem = 0;
	state->skipped = 0;
	state->pages = 0;

	/* Reset rather than abandon: see batch_cxt in vfdw_scan_internal.h. */
	vfdw_scan_batch_resets++;
	MemoryContextReset(state->batch_cxt);
	state->batch = vfdw_batch_begin(state->vconn, state->batch_cxt);

	/*
	 * A search runs again rather than rewinding. Its query vector may be a
	 * Param, so the second pass of a nested loop may be a different question
	 * - and replaying the first pass's rows would answer it with the first
	 * pass's answer, which is the failure a rescan exists to avoid.
	 */
	vfdw_search_reset(&state->knn);
}

void
vfdw_scan_end(ForeignScanState *node)
{
	VfdwScanState *state = (VfdwScanState *) node->fdw_state;

	if (state == NULL)
		return;

	/*
	 * Draining tells the pool the connection is clean; the batch's reset
	 * callback would release the replies either way, but a connection with
	 * unread replies still queued cannot be handed to the next statement.
	 */
	vfdw_batch_end(state->batch);
	vfdw_release_connection(state->vconn);
	node->fdw_state = NULL;
}

int64
vfdw_scan_skipped(ForeignScanState *node)
{
	VfdwScanState *state = (VfdwScanState *) node->fdw_state;

	return state != NULL ? state->skipped : 0;
}

int64
vfdw_scan_pages(ForeignScanState *node)
{
	VfdwScanState *state = (VfdwScanState *) node->fdw_state;

	return state != NULL ? state->pages : 0;
}

/*
 * Finish with a scan state that was not driven by the executor.
 *
 * Drains and releases as vfdw_scan_end does; kept here rather than in
 * vfdw_analyze.c so that VfdwScanState stays opaque outside this file.
 */
void
vfdw_scan_state_close(VfdwScanState *state)
{
	vfdw_batch_end(state->batch);
	vfdw_release_connection(state->vconn);
}
