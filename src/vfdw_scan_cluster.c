/*-------------------------------------------------------------------------
 *
 * vfdw_scan_cluster.c
 *		Visiting every primary, because SCAN is per node.
 *
 * INVARIANT I5, IN ITS CLUSTER FORM. "No tuple now" is not "scan over", and
 * here that sharpens: AN EXHAUSTED NODE IS NOT AN EXHAUSTED SCAN. A cursor
 * reaching 0 means one primary is done, and treating it as the end of the
 * traversal returns whichever third of the keyspace happened to be visited
 * first - a plausible, wrong answer that no error accompanies. That is the
 * single most dangerous bug available in this slice, so `cursor == NULL` is
 * deliberately no longer sufficient to end a scan.
 *
 * Nodes are visited ONE AT A TIME and each connection is given back before
 * the next is taken. Holding all of them would cost one lease per primary
 * for the whole scan, and VFDW_MAX_CONN_SLOTS is per identity - a query
 * joining a few foreign tables on a large cluster would exhaust the pool for
 * no benefit, because rows are emitted as they are read and nothing needs two
 * nodes open at once.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_scan_cluster.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "vfdw.h"

#include "foreign/foreign.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "vfdw_cluster.h"
#include "vfdw_scan_cluster.h"
#include "vfdw_scan_internal.h"

/*
 * The scan's identity, refetched rather than held.
 *
 * A ForeignServer and a UserMapping belong to contexts that a scan outlives,
 * so the Oids travel and the catalog answers again when a node change needs
 * them. Both are cached by syscache, so this is not a round trip.
 */
ForeignServer *
vfdw_scan_server(VfdwScanState *state)
{
	return GetForeignServer(state->serverid);
}

UserMapping *
vfdw_scan_user(VfdwScanState *state)
{
	ForeignServer *server = GetForeignServer(state->serverid);

	return GetUserMapping(GetUserId(), server->serverid);
}

/*
 * Take the node list for this scan, or report that there is nothing to fan
 * out over.
 *
 * The list is COPIED into the scan's own context rather than pointed at. The
 * map is a cache and may be reloaded by a MOVED on some other statement, and
 * a scan halfway through node two must not have the ground move under it -
 * it would either skip a primary or visit one twice, and both are wrong
 * answers rather than errors.
 */
/*
 * Take our own copy of the node list.
 *
 * Pointing at the map would be cheaper and wrong: the map is a cache another
 * statement may reload, and a scan halfway through node two must not have it
 * move - it would skip a primary or visit one twice, both wrong answers
 * rather than errors.
 */
void
vfdw_scan_cluster_copy_nodes(VfdwScanState *state, const VfdwSlotMap *map)
{
	MemoryContext old = MemoryContextSwitchTo(state->scan_cxt);
	int			i;

	state->nnodes = map->nnodes;
	state->nodes = palloc(sizeof(VfdwClusterNode) * Max(map->nnodes, 1));
	for (i = 0; i < map->nnodes; i++)
	{
		state->nodes[i].host = pstrdup(map->nodes[i].host);
		state->nodes[i].port = map->nodes[i].port;
	}
	MemoryContextSwitchTo(old);
}

bool
vfdw_scan_cluster_setup(VfdwScanState *state, ForeignServer *server,
						UserMapping *user)
{
	const VfdwSlotMap *map;

	state->fanout = false;
	state->cur_node = 0;

	if (!vfdw_conn_is_cluster(state->vconn))
		return false;

	map = vfdw_cluster_map(state->vconn);

	vfdw_scan_cluster_copy_nodes(state, map);
	state->pass_map = map;
	state->fanout = true;

	/*
	 * How many times a scan may chase redirected keys before giving up.
	 *
	 * Bounded because two nodes mid-migration can point at each other, and an
	 * unbounded chase would be a hang rather than an error - the failure this
	 * budget exists to convert into a finite, reportable one. Three is enough
	 * for a slot that moves while a scan is running; a cluster resharding
	 * faster than that is not one a single scan can photograph anyway.
	 */
	state->redirect_passes = 3;

	/*
	 * The seed connection is not necessarily a primary, and even when it is,
	 * it is not necessarily the first one in the list. Moving onto node 0
	 * explicitly means the fan-out visits exactly the primaries the map names,
	 * in a known order, whoever answered the discovery.
	 */
	vfdw_scan_cluster_attach(state, server, user, 0);
	return true;
}

/*
 * Point the scan at node `n`, releasing whatever it held.
 *
 * The batch is rebuilt because it belongs to a connection: reusing one across
 * a change of peer would pair this node's replies with the last node's
 * outstanding commands.
 */
void
vfdw_scan_cluster_attach(VfdwScanState *state, ForeignServer *server,
						 UserMapping *user, int n)
{
	Assert(n < state->nnodes);

	if (state->batch != NULL)
		vfdw_batch_end(state->batch);
	if (state->vconn != NULL)
		vfdw_release_connection(state->vconn);

	state->cur_node = n;
	state->vconn = vfdw_get_connection_node(server, user,
											state->nodes[n].host,
											state->nodes[n].port);
	/*
	 * The batch's own context, reset first. This runs on every node advance
	 * and on every redirect, not only at setup, so it is the site that
	 * produces the most batches by far - allocating them in the scan context
	 * left a struct and a reset callback per node per pass, for the whole
	 * query. vfdw_batch_end above has already released the reply, so there is
	 * nothing live in here to reset out from under.
	 */
	MemoryContextReset(state->batch_cxt);
	state->batch = vfdw_batch_begin(state->vconn, state->batch_cxt);

	/*
	 * A fresh traversal on the new node. `started` is what tells the page
	 * loop to send cursor 0 rather than the last node's cursor - leaving it
	 * set would resume node n at a position that meant something on node
	 * n-1, which is not an error anywhere, just a different set of keys.
	 */
	state->started = false;
	if (state->cursor != NULL)
	{
		pfree(state->cursor);
		state->cursor = NULL;
	}
}

/*
 * Move to the next primary, or report that every one has been visited.
 *
 * THIS IS THE FUNCTION I5 IS ABOUT. Returning false here is the only thing
 * that may end a keyspace scan on a cluster.
 */
bool
vfdw_scan_cluster_advance(VfdwScanState *state, ForeignServer *server,
						  UserMapping *user)
{
	if (!state->fanout || state->cur_node + 1 >= state->nnodes)
		return false;

	vfdw_scan_cluster_attach(state, server, user, state->cur_node + 1);
	return true;
}

/*
 * The subset of the plan's keys that this node owns.
 *
 * A keyed lookup names its keys up front, and on a cluster they do not share
 * a node. Asking one primary for all of them earns a MOVED for every key it
 * does not hold, so the list is partitioned and each node is asked only for
 * its own - which is also why an empty subset must be normal here rather than
 * the end of the scan.
 */
void
vfdw_scan_cluster_keys_for_node(VfdwScanState *state)
{
	const VfdwClusterNode *node = &state->nodes[state->cur_node];
	MemoryContext old = MemoryContextSwitchTo(state->page_cxt);
	int			i;

	state->keys = palloc(sizeof(char *) * Max(state->n_plan_keys, 1));
	state->keylens = palloc(sizeof(size_t) * Max(state->n_plan_keys, 1));
	state->nkeys = 0;

	for (i = 0; i < state->n_plan_keys; i++)
	{
		const VfdwClusterNode *owner;

		owner = vfdw_cluster_route(state->pass_map,
								   state->plan_keys[i],
								   state->plan_keylens[i]);

		/*
		 * An unplaced slot is asked of the node we are on rather than
		 * dropped: invariant I8 says the map is a cache, so "nobody claims
		 * it" means ask and expect a redirect, never "the key does not
		 * exist". Dropping it would turn a resharding window into missing
		 * rows.
		 */
		if (owner != NULL &&
			(owner->port != node->port ||
			 strcmp(owner->host, node->host) != 0))
			continue;

		state->keys[state->nkeys] = state->plan_keys[i];
		state->keylens[state->nkeys] = state->plan_keylens[i];
		state->nkeys++;
	}

	state->next_key = 0;
	MemoryContextSwitchTo(old);
}

/*
 * Note a key the server says is elsewhere.
 *
 * MOVED means the slot has a new owner, so the map is dropped and the next
 * lookup reloads it. ASK means only this one key has already migrated while
 * ownership has not changed - refreshing on that would rewrite the whole
 * routing table from a transient state, so it is deliberately not done.
 * Either way the key is remembered and fetched again once the current pass
 * is over.
 */
void
vfdw_scan_redirected(VfdwScanState *state, const valkeyReply *reply,
					 const char *key, size_t keylen)
{
	MemoryContext old = MemoryContextSwitchTo(state->scan_cxt);
	int			n = state->n_redirected;

	/* repalloc rejects NULL, and the first redirect of a scan always finds it. */
	state->redirected = n == 0
		? palloc(sizeof(char *))
		: repalloc(state->redirected, sizeof(char *) * (n + 1));
	state->redirected_lens = n == 0
		? palloc(sizeof(size_t))
		: repalloc(state->redirected_lens, sizeof(size_t) * (n + 1));
	state->redirected[n] = pnstrdup(key, keylen);
	state->redirected_lens[n] = keylen;
	state->n_redirected = n + 1;
	MemoryContextSwitchTo(old);

	if (!vfdw_cluster_redirect_is_ask(reply))
		vfdw_cluster_invalidate(state->serverid,
								GetUserMapping(GetUserId(),
											   state->serverid)->userid);
}

/*
 * Begin a pass over the keys that were redirected, if any are owed one.
 *
 * The node list is rebuilt from the refreshed map: a slot moving between two
 * existing primaries leaves the set unchanged, but a node joining does not,
 * and chasing a key onto a node this scan has never heard of is the whole
 * point of the pass.
 */
bool
vfdw_scan_retry_pass(VfdwScanState *state)
{
	/* Nothing is owed a pass, so the scan is genuinely over. */
	if (state->n_redirected == 0)
		return false;

	/*
	 * OWED A PASS AND OUT OF BUDGET IS NOT THE SAME ANSWER, and returning it
	 * as one is invariant I5 run backwards.
	 *
	 * These two conditions were a single test, so a scan that gave up chasing
	 * keys ended exactly like a scan that had nothing left to chase: the rows
	 * for every redirected key simply were not in the result, and nothing was
	 * reported. That is silent truncation through the slot map instead of the
	 * cursor, and the budget makes it a NORMAL outcome rather than a rare one
	 * - a cluster still gossiping a slot it has just been told about answers
	 * MOVED to the reload as readily as to the original fetch, so three passes
	 * are spent while the cluster is merely converging.
	 *
	 * It was observed as a keyed lookup returning zero rows on roughly one
	 * cluster formation in six, with no error and nothing in the log. A
	 * budget is still right - two nodes mid-migration can point at each other
	 * and an unbounded chase is a hang - but exhausting it is a failure to
	 * answer, and it says so.
	 */
	if (state->redirect_passes <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("gave up following Valkey Cluster redirects for %d key(s)",
						state->n_redirected),
				 errdetail("The slot map was refreshed and the same keys were "
						   "redirected again on every attempt, which is what a "
						   "cluster still agreeing about a slot looks like from "
						   "one client."),
				 errhint("Retry the statement. If it persists, the cluster is "
						 "not converging: check CLUSTER INFO on every primary.")));

	state->redirect_passes--;
	state->plan_keys = state->redirected;
	state->plan_keylens = state->redirected_lens;
	state->n_plan_keys = state->n_redirected;

	state->redirected = NULL;
	state->redirected_lens = NULL;
	state->n_redirected = 0;

	state->retry_pass = true;
	state->started = false;

	if (state->fanout)
	{
		state->pass_map = vfdw_cluster_map(state->vconn);
		vfdw_scan_cluster_copy_nodes(state, state->pass_map);
		vfdw_scan_cluster_attach(state, vfdw_scan_server(state),
								 vfdw_scan_user(state), 0);
	}
	return true;
}

/*
 * One page for a strategy whose keys the plan already named.
 *
 * Node-aware, so it lives here rather than in the generic page loop: on a
 * cluster the named keys do not share a node, each primary is asked only for
 * the ones it owns, and A NODE CONTRIBUTING NOTHING IS ORDINARY - it must
 * advance rather than end the scan. That is the same I5 trap as an exhausted
 * cursor, reached by a different route.
 *
 * Off a cluster this is the original single pass: fetch every named key at
 * once, and the scan is over.
 */
bool
vfdw_scan_keys_page(VfdwScanState *state)
{
	if (state->n_plan_keys == 0)
		return false;

	/*
	 * On a cluster the named keys do not share a node, so each primary is
	 * asked only for the ones it owns and a node contributing NOTHING is
	 * ordinary - it must advance rather than end the scan. That is the
	 * same I5 trap as an exhausted cursor, reached by a different route.
	 */
	while (!state->started || state->fanout)
	{
		if (state->started &&
			!vfdw_scan_cluster_advance(state, vfdw_scan_server(state),
									   vfdw_scan_user(state)))
			return false;
		state->started = true;

		MemoryContextReset(state->page_cxt);
		if (state->fanout)
			vfdw_scan_cluster_keys_for_node(state);
		else
		{
			state->keys = state->plan_keys;
			state->keylens = state->plan_keylens;
			state->nkeys = state->n_plan_keys;
			state->next_key = 0;
		}

		if (state->nkeys > 0)
		{
			vfdw_scan_queue_page(state);
			return true;
		}
	}
	return false;
}
