/*-------------------------------------------------------------------------
 *
 * vfdw_scan.c
 *		Reading rows out of Valkey.
 *
 * Two structural decisions shape this file.
 *
 * Keys are discovered a page at a time, and the values for a whole page are
 * fetched in one pipelined batch. A round trip per row would run the scan at
 * the latency of the link rather than at the speed of the server.
 *
 * "No tuple this call" and "the scan is over" are separate states. A key that
 * vanished between the SCAN and the fetch, or that holds the wrong type,
 * advances the producer; only an exhausted cursor with nothing left in flight
 * ends the scan. Conflating the two makes a missing value at a page boundary
 * silently truncate the result (invariant I5).
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_scan.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_scan.h"

#include "common/hashfn.h"
#include "utils/hsearch.h"

#include "vfdw_overlay.h"
#include "vfdw_cluster.h"
#include "vfdw_scan_cluster.h"
#include "vfdw_scan_internal.h"
#include "vfdw_scan_overlay.h"

#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "vfdw_cmd.h"
#include "vfdw_error.h"
#include "vfdw_map.h"
#include "vfdw_row.h"
#include "vfdw_ttl.h"

/* The Valkey type name for SCAN's server-side TYPE filter. */
static const char *
vfdw_scan_type_filter(VfdwTableType type)
{
	switch (type)
	{
		case VFDW_TABLE_HASH:	return "hash";
		case VFDW_TABLE_LIST:	return "list";
		case VFDW_TABLE_SET:	return "set";
		case VFDW_TABLE_ZSET:	return "zset";
		case VFDW_TABLE_STRING:
		default:				return "string";
	}
}

/*
 * Build the SCAN for the next page.
 */
static void
vfdw_scan_build_scan(VfdwScanState *state, VfdwCmd *cmd)
{
	const VfdwServerOptions *opts = vfdw_conn_options(state->vconn);

	vfdw_cmd_init(cmd, state->page_cxt, 8);

	/*
	 * A keyset names the table's keys in a set of its own, so the whole
	 * keyspace never has to be walked. SSCAN takes the set first and the
	 * cursor second - the reverse of SCAN - and accepts neither MATCH nor
	 * TYPE, so wrong-type members are filtered when their values come back.
	 */
	if (state->map->keyset != NULL)
	{
		vfdw_cmd_add_cstr(cmd, "SSCAN");
		vfdw_cmd_add_cstr(cmd, state->map->keyset);
		vfdw_cmd_add_cstr(cmd, state->started ? state->cursor : "0");
		vfdw_cmd_add_cstr(cmd, "COUNT");
		vfdw_cmd_add_int(cmd, opts->scan_count);
		return;
	}

	vfdw_cmd_add_cstr(cmd, "SCAN");
	vfdw_cmd_add_cstr(cmd, state->started ? state->cursor : "0");

	/*
	 * The pattern is built by the planner, which already combined the table's
	 * keyprefix with whatever a LIKE on the key narrowed it to. Rebuilding it
	 * here would give two places that could disagree about which keys the
	 * table contains.
	 */
	if (state->pattern != NULL)
	{
		vfdw_cmd_add_cstr(cmd, "MATCH");
		vfdw_cmd_add_cstr(cmd, state->pattern);
	}

	vfdw_cmd_add_cstr(cmd, "COUNT");
	vfdw_cmd_add_int(cmd, opts->scan_count);

	/*
	 * Filter by type on the server. Fetching every key and discarding the
	 * mismatches client-side is both wasteful and the mechanism by which a
	 * wrong-type key at a page boundary truncates the result.
	 */
	vfdw_cmd_add_cstr(cmd, "TYPE");
	vfdw_cmd_add_cstr(cmd, vfdw_scan_type_filter(state->map->tabletype));
}

/*
 * Insist that an element of a SCAN reply is a bulk string.
 *
 * vfdw_reply_expect validates the TOP level of a reply and nothing inside it,
 * so without this an integer or nil element would be read as (str, len) =
 * (NULL, 0): pnstrdup(NULL, 0) and memcpy(dst, NULL, 0) are undefined by the
 * standard however benign glibc makes them, and the empty cursor or empty key
 * that resulted would then be sent back to the server. libvalkey parses
 * faithfully whatever the far end sends, so "the server would not do that" is
 * an assumption about the network and not about the library.
 */
static void
vfdw_scan_expect_string(const valkeyReply *elem, const char *what)
{
	if (elem->type == VALKEY_REPLY_STRING)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
			 errmsg("%s", what),
			 errdetail("Valkey answered with an element of type %s where a "
					   "string was required.",
					   vfdw_reply_type_name(elem->type))));
}

static void
vfdw_scan_take_keys(VfdwScanState *state, const valkeyReply *keys)
{
	MemoryContext old;
	int			i;

	state->nkeys = (int) keys->elements;
	if (state->nkeys == 0)
		return;

	old = MemoryContextSwitchTo(state->page_cxt);

	state->keys = palloc(sizeof(char *) * state->nkeys);
	state->keylens = palloc(sizeof(size_t) * state->nkeys);
	for (i = 0; i < state->nkeys; i++)
	{
		const valkeyReply *k = vfdw_reply_child(keys, (size_t) i);

		vfdw_scan_expect_string(k, "SCAN returned a key that is not a string");

		/* Not pnstrdup: see the note above vfdw_scan_take_page. */
		state->keys[i] = palloc(k->len + 1);
		memcpy(state->keys[i], k->str, k->len);
		state->keys[i][k->len] = '\0';
		state->keylens[i] = k->len;
	}

	MemoryContextSwitchTo(old);
}

/*
 * Record the cursor and key list from a SCAN reply.
 *
 * Keys are copied with palloc and memcpy rather than pnstrdup, which does
 * strnlen(in, len) first: a key containing a NUL byte would be copied short
 * while keylens still recorded its full length, so everything downstream read
 * past the allocation and the row vanished. A Valkey key is a binary-safe byte
 * string and nothing here may assume otherwise (invariant I3).
 */
static void
vfdw_scan_take_page(VfdwScanState *state, valkeyReply *reply)
{
	const valkeyReply *keys;

	if (reply->elements != 2)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
				 errmsg("SCAN returned %d elements, expected 2",
						(int) reply->elements)));

	state->started = true;

	/* A cursor of "0" means the traversal is complete. */
	{
		const valkeyReply *cur = vfdw_reply_child(reply, 0);
		MemoryContext old;

		vfdw_scan_expect_string(cur, "SCAN returned a cursor that is not a string");

		old = MemoryContextSwitchTo(state->scan_cxt);

		if (state->cursor != NULL)
			pfree(state->cursor);

		if (cur->len == 1 && cur->str[0] == '0')
			state->cursor = NULL;
		else
			state->cursor = pnstrdup(cur->str, cur->len);

		MemoryContextSwitchTo(old);
	}

	keys = vfdw_reply_child(reply, 1);
	if (keys->type != VALKEY_REPLY_ARRAY)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
				 errmsg("SCAN did not return a list of keys")));

	vfdw_scan_take_keys(state, keys);
}

static bool
vfdw_scan_next_page(VfdwScanState *state)
{
	VfdwCmd		cmd;
	valkeyReply *reply;

	/*
	 * AN EXHAUSTED NODE IS NOT AN EXHAUSTED SCAN (invariant I5). A cursor of
	 * 0 ends this primary's traversal; only running out of primaries ends the
	 * scan.
	 */
	if (state->started && state->cursor == NULL &&
		!vfdw_scan_cluster_advance(state, vfdw_scan_server(state),
								   vfdw_scan_user(state)))
		return false;

	MemoryContextReset(state->page_cxt);
	state->keys = NULL;
	state->nkeys = 0;
	state->next_key = 0;

	vfdw_scan_build_scan(state, &cmd);
	vfdw_batch_add(state->batch, &cmd);
	state->pages++;

	reply = vfdw_batch_next(state->batch);
	vfdw_reply_expect(reply, VFDW_RTYPE(VALKEY_REPLY_ARRAY), "SCAN failed");

	vfdw_scan_take_page(state, reply);
	return true;
}

/*
 * Make more replies available, or report that there are none left.
 */
static bool
vfdw_scan_refill(VfdwScanState *state)
{
	/*
	 * The plan named the keys, so there is nothing to discover: fetch them
	 * all in one batch and then the scan is over. A list narrowed to nothing
	 * - every named key lay outside the table's keyprefix - ends immediately
	 * rather than falling back to a keyspace walk.
	 */
	if (state->retry_pass || state->strategy != VFDW_SCAN_KEYSPACE)
	{
		if (vfdw_scan_keys_page(state))
			return true;
		return vfdw_scan_retry_pass(state) && vfdw_scan_keys_page(state);
	}

	/*
	 * Keep asking until a page actually yields keys. An empty page is not the
	 * end of the scan, and returning here on one drops the tail of the
	 * result set.
	 */
	for (;;)
	{
		if (!vfdw_scan_next_page(state))
			return vfdw_scan_retry_pass(state) && vfdw_scan_keys_page(state);
		if (state->nkeys > 0)
		{
			vfdw_scan_queue_page(state);
			return true;
		}
	}
}

/*
 * A REDIRECT IS NOT AN ABSENT ROW: MOVED and ASK say the row EXISTS and
 * lives elsewhere, so it is recorded here for the retry pass and the rule
 * below advances past it. Reported as an error it aborts a scan that is
 * merely mid-reshard; taken as a missing row it loses data and says nothing.
 */
static void
vfdw_scan_note_redirect(VfdwScanState *state, const valkeyReply *reply,
						const char *key, size_t keylen)
{
	if (vfdw_cluster_is_redirect(reply))
		vfdw_scan_redirected(state, reply, key, keylen);
}

/*
 * A reply that carries no usable row: the key expired between the SCAN and
 * the fetch, holds a type this table does not read, or was just recorded as
 * living on another node. WRONGTYPE is the only error of the three, and only
 * because a keyset table's SSCAN takes neither MATCH nor TYPE (see
 * vfdw_scan_build_scan) and its members may be of any type. Admitting the
 * rest made a NOPERM or an OOM one more skipped key and a result set silently
 * short of it - truncation through the value fetch rather than through the
 * cursor, which I5 forbids both ways.
 */
static bool
vfdw_scan_reply_is_skippable(const valkeyReply *reply)
{
	return reply->type == VALKEY_REPLY_NIL ||
		vfdw_cluster_is_redirect(reply) ||
		vfdw_reply_has_prefix(reply, "WRONGTYPE");
}

/*
 * The shapes a value fetch can answer with. An error is reported under the
 * SQLSTATE its prefix implies; a STATUS is refused rather than skipped, since
 * none of GET, HGETALL, LRANGE, SMEMBERS and ZRANGE has one among its answers
 * and one arriving means the reply stream is at an offset this scan cannot
 * account for. RESP2 sends an array where RESP3 sends a map or a set.
 */
static void
vfdw_scan_expect_value(const VfdwScanState *state, valkeyReply *reply)
{
	int			allowed = state->map->tabletype == VFDW_TABLE_STRING
		? VFDW_RTYPE(VALKEY_REPLY_STRING)
		: VFDW_RTYPE(VALKEY_REPLY_ARRAY) | VFDW_RTYPE(VALKEY_REPLY_MAP) |
		VFDW_RTYPE(VALKEY_REPLY_SET);

	vfdw_reply_expect(reply, allowed, "could not read a value from Valkey");
}

/*
 * A container reply with nothing in it: the key is not there.
 *
 * Only GET says so with a nil. HGETALL on a key that does not exist answers
 * with an empty map under RESP3 and an empty array under RESP2, and
 * LRANGE/SMEMBERS/ZRANGE answer with an empty container - none of which is
 * skippable by shape. The collection types survived that because
 * vfdw_scan_take_collection turns zero elements into zero rows anyway, but a
 * hash table has no member loop: vfdw_scan_fill would emit one row with the
 * key column set to the name that was asked for and every mapped field NULL.
 * So 'SELECT EXISTS (SELECT 1 FROM h WHERE k = <anything>)' answered true, an
 * IN list returned a row per named key present or not, and a keyset hash
 * table carrying a stale member returned a phantom all-NULL row on a plain
 * unqualified SELECT which ANALYZE then counted. That is invariant I5 run
 * backwards - "no tuple now" turned into a tuple.
 *
 * The boundary is exact and must stay there: a hash that EXISTS but holds
 * only fields this table does not map answers with a NON-empty reply and
 * legitimately produces a row whose mapped columns are all NULL. Emptiness of
 * the reply, not emptiness of the row, is what says the key is absent.
 *
 * Counted as skipped for the same reason a wrong-type key is: the key was
 * discovered and then produced nothing, and that is otherwise invisible.
 */
static bool
vfdw_scan_reply_is_absent(const VfdwScanState *state, const valkeyReply *reply)
{
	if (state->map->tabletype == VFDW_TABLE_STRING)
		return false;

	if (reply->type != VALKEY_REPLY_ARRAY &&
		reply->type != VALKEY_REPLY_MAP &&
		reply->type != VALKEY_REPLY_SET)
		return false;

	/*
	 * HMGET NEVER ANSWERS WITH AN EMPTY ARRAY. It returns one entry per field
	 * asked for, nil where the field is not there, so the rule below cannot
	 * read it: a key deleted between the SCAN that named it and the fetch
	 * answers all-nil, and so does a key that is present and holds none of
	 * this table's fields.
	 *
	 * READING ALL-NIL AS ABSENT WOULD BE WRONG, and wrong against a boundary
	 * this tree states and asserts: a hash that exists holding only fields the
	 * table does not map is a row, with those columns NULL. That is the
	 * far side of the phantom-row defect - a row must not be invented for a
	 * key that is gone, and must not be withheld from a key that is there.
	 *
	 * HLEN is what tells them apart, asked in the same batch. Its answer is
	 * this key's field count; zero is the empty hash Valkey does not keep, so
	 * zero means the key is gone. -1 is "nothing asked", which no HMGET table
	 * reaches and which reads as present.
	 */
	if (state->map->hmget)
		return state->cur_hlen == 0;

	return reply->elements == 0;
}

/*
 * Does this key produce no row, without ending the scan (I5)?
 *
 * Three unrelated reasons with one consequence: the key is not in the table's
 * keyset, the reply is one the scan skips, or the key is not there at all.
 * Gathered because the caller does the same thing with each - advance, and go
 * on - and separating them would only invite a fourth to be handled somewhere
 * that forgets to advance.
 */
static bool
vfdw_scan_produces_nothing(VfdwScanState *state, const valkeyReply *reply)
{
	return state->cur_member == 0 ||
		vfdw_scan_reply_is_skippable(reply) ||
		vfdw_scan_reply_is_absent(state, reply);
}

/*
 * Emit the next member of a collection reply, if any remain.
 */
static bool
vfdw_scan_emit_member(VfdwScanState *state, TupleTableSlot *slot)
{
	int			stride;

	if (state->cur_elem >= (int) state->cur_reply->elements)
	{
		state->cur_reply = NULL;
		return false;
	}

	stride = vfdw_scan_member_stride(state->map->tabletype, state->cur_reply);
	state->rowctx.cur_elem = state->cur_elem;
	vfdw_scan_fill(&state->rowctx, slot, state->cur_key, state->cur_keylen,
				   state->cur_reply);
	state->cur_elem += stride;
	return true;
}

/*
 * Hold a collection reply so its members can become rows.
 *
 * A reply that reaches here has at least one element - an empty one means the
 * key is gone and was counted as skipped before this - but the zero-element
 * exit in vfdw_scan_emit_member stays: it is what ends each key's run of
 * members without ending the scan, and a collection emptied between the
 * discovery and the fetch would otherwise emit a row per absent member.
 */
static void
vfdw_scan_take_collection(VfdwScanState *state, const char *key,
						  size_t keylen, valkeyReply *reply)
{
	state->cur_reply = reply;
	state->cur_elem = 0;
	state->cur_key = key;
	state->cur_keylen = keylen;
}

/*
 * Produce the next row, or leave the slot empty when there are none left.
 *
 * Takes the scan state rather than the executor node so that ANALYZE can
 * drive the same producer.
 */
TupleTableSlot *
vfdw_scan_fetch(VfdwScanState *state, TupleTableSlot *slot)
{
	ExecClearTuple(slot);

	for (;;)
	{
		valkeyReply *reply;
		const char *key;
		size_t		keylen;

		CHECK_FOR_INTERRUPTS();

		/* A collection key is still unpacking; its reply must stay put. */
		if (state->cur_reply != NULL && vfdw_scan_emit_member(state, slot))
			return slot;

		if (state->next_key >= state->nkeys)
		{
			if (!vfdw_scan_refill(state))
				return vfdw_scan_drain_tail(state, slot);
			continue;
		}

		key = state->keys[state->next_key];
		keylen = state->keylens[state->next_key];
		state->next_key++;

		/* Taken FIRST, always: see vfdw_scan_apply_overlay. */
		reply = vfdw_scan_take_replies(state);
		if (reply == NULL)
			return slot;

		switch (vfdw_scan_apply_overlay(state, key, keylen, slot))
		{
			case VFDW_OVL_REPLACE:
				return slot;	/* this transaction rewrote it */
			case VFDW_OVL_HIDE:
				continue;		/* it deleted it: no tuple now, scan goes on */
			case VFDW_OVL_PASS:
				break;			/* the server's row stands */
		}

		vfdw_scan_note_redirect(state, reply, key, keylen);
		if (vfdw_scan_produces_nothing(state, reply))
		{
			state->skipped++;
			continue;			/* advance, do not end the scan */
		}

		vfdw_scan_expect_value(state, reply);
		if (vfdw_scan_is_multirow(state->map))
		{
			vfdw_scan_take_collection(state, key, keylen, reply);
			continue;
		}

		vfdw_scan_fill(&state->rowctx, slot, key, keylen, reply);
		return slot;
	}
}

TupleTableSlot *
vfdw_scan_next(ForeignScanState *node)
{
	return vfdw_scan_fetch((VfdwScanState *) node->fdw_state,
						   node->ss.ss_ScanTupleSlot);
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
	MemoryContextReset(state->batch_cxt);
	state->batch = vfdw_batch_begin(state->vconn, state->batch_cxt);
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
