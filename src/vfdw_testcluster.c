/*-------------------------------------------------------------------------
 *
 * vfdw_testcluster.c
 *		The slot map, as SQL rows.
 *
 * Routing has no output of its own: a correctly routed command and a
 * misrouted one both come back with an answer, and the misrouted one comes
 * back with someone else's. So the map has to be readable directly, or the
 * only assertions available are the ones that cannot fail.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_testcluster.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "vfdw.h"

#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"

#include "vfdw_cluster.h"
#include "vfdw_cmd.h"
#include "vfdw_conn.h"
#include "vfdw_test_common.h"
#include "vfdw_val.h"

PG_FUNCTION_INFO_V1(valkey_fdw_test_cluster_map);
PG_FUNCTION_INFO_V1(valkey_fdw_test_cluster_route);
PG_FUNCTION_INFO_V1(valkey_fdw_test_cluster_nodes);

#define VFDW_CLUSTER_MAP_NCOLS		4
#define VFDW_CLUSTER_ROUTE_NCOLS	3
#define VFDW_CLUSTER_NODES_NCOLS	4

/*
 * The map, with the connection given straight back.
 *
 * The lease matters: taking a slot and not returning it costs one of
 * VFDW_MAX_CONN_SLOTS per call, so a query calling this once per row exhausts
 * the pool and fails with "too many concurrent Valkey scans" - which is what
 * the first version of this function did, and what a fifty-key routing
 * assertion caught. The map itself lives in its own backend-lifetime context
 * and does not depend on the connection that loaded it.
 */
static const VfdwSlotMap *
vfdw_testcluster_map(text *servername)
{
	ForeignServer *server = vfdw_test_server(text_to_cstring(servername));
	UserMapping *user = GetUserMapping(GetUserId(), server->serverid);
	VfdwConn   *vconn = vfdw_get_connection_cluster(server, user);
	const VfdwSlotMap *map = vfdw_cluster_map(vconn);

	vfdw_release_connection(vconn);
	return map;
}

/*
 * valkey_fdw_test_cluster_map(server) -> (slot_start, slot_end, host, port)
 *
 * One row per CONTIGUOUS RANGE rather than per slot. Sixteen thousand rows
 * would be unreadable in an expected file and would make every reshard a
 * diff; ranges are how the server states it too, so a recorded output stays
 * meaningful to someone reading it.
 *
 * Slots nobody claims are omitted rather than reported with a null owner: an
 * incompletely formed cluster is exactly when the covered count matters, and
 * a suite should assert that count rather than read it out of gaps.
 */
Datum
valkey_fdw_test_cluster_map(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const VfdwSlotMap *map = vfdw_testcluster_map(PG_GETARG_TEXT_PP(0));
	int			i = 0;

	InitMaterializedSRF(fcinfo, 0);

	while (i < VFDW_CLUSTER_NSLOTS)
	{
		Datum		values[VFDW_CLUSTER_MAP_NCOLS];
		bool		nulls[VFDW_CLUSTER_MAP_NCOLS] = {false, false, false, false};
		int16		owner = map->owner[i];
		int			start = i;

		if (owner < 0)
		{
			i++;
			continue;
		}

		while (i < VFDW_CLUSTER_NSLOTS && map->owner[i] == owner)
			i++;

		values[0] = Int32GetDatum(start);
		values[1] = Int32GetDatum(i - 1);
		values[2] = CStringGetTextDatum(map->nodes[owner].host);
		values[3] = Int32GetDatum(map->nodes[owner].port);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

/*
 * valkey_fdw_test_cluster_route(server, key) -> (slot, host, port)
 *
 * Where this wrapper would send a command for this key.
 *
 * Host and port are NULL when nothing claims the slot, which is invariant I8
 * showing through: an unplaced slot is a reason to ask a node and be
 * redirected, not an error, so it must be representable rather than raise.
 */
Datum
valkey_fdw_test_cluster_route(PG_FUNCTION_ARGS)
{
	const VfdwSlotMap *map = vfdw_testcluster_map(PG_GETARG_TEXT_PP(0));
	bytea	   *key = PG_GETARG_BYTEA_PP(1);
	const VfdwClusterNode *node;
	TupleDesc	tupdesc;
	Datum		values[VFDW_CLUSTER_ROUTE_NCOLS];
	bool		nulls[VFDW_CLUSTER_ROUTE_NCOLS] = {false, false, false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	node = vfdw_cluster_route(map, VARDATA_ANY(key), VARSIZE_ANY_EXHDR(key));

	values[0] = Int32GetDatum((int) vfdw_val_slot(VARDATA_ANY(key),
												 VARSIZE_ANY_EXHDR(key)));
	if (node == NULL)
		nulls[1] = nulls[2] = true;
	else
	{
		values[1] = CStringGetTextDatum(node->host);
		values[2] = Int32GetDatum(node->port);
	}

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * valkey_fdw_test_cluster_nodes(server) -> (host, port, reachable, myself)
 *
 * Open a POOLED connection to each primary in the map and ask it who it is.
 *
 * This is the observable for per-node pooling, and it has to reach each node
 * individually or it proves nothing: a pool that ignored the node and handed
 * back the seed connection every time would answer "reachable" three times
 * over while only ever talking to one server. So each row carries the node id
 * the server reports for ITSELF, from CLUSTER MYID - three distinct ids is
 * three distinct peers, and no amount of pooling confusion can fake that.
 */
Datum
valkey_fdw_test_cluster_nodes(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	text	   *servername = PG_GETARG_TEXT_PP(0);
	ForeignServer *server = vfdw_test_server(text_to_cstring(servername));
	UserMapping *user = GetUserMapping(GetUserId(), server->serverid);
	const VfdwSlotMap *map = vfdw_testcluster_map(servername);
	int			i;

	InitMaterializedSRF(fcinfo, 0);

	for (i = 0; i < map->nnodes; i++)
	{
		Datum		values[VFDW_CLUSTER_NODES_NCOLS];
		bool		nulls[VFDW_CLUSTER_NODES_NCOLS] = {false, false, false,
		false};
		VfdwConn   *vconn;
		MemoryContext mctx;
		VfdwBatch  *batch;
		VfdwCmd		cmd;
		valkeyReply *reply;

		CHECK_FOR_INTERRUPTS();

		mctx = AllocSetContextCreate(CurrentMemoryContext, "vfdw cluster node",
									 ALLOCSET_SMALL_SIZES);

		vconn = vfdw_get_connection_node(server, user, map->nodes[i].host,
										 map->nodes[i].port);
		batch = vfdw_batch_begin(vconn, mctx);
		vfdw_cmd_init(&cmd, mctx, 2);
		vfdw_cmd_add_cstr(&cmd, "CLUSTER");
		vfdw_cmd_add_cstr(&cmd, "MYID");
		vfdw_batch_add(batch, &cmd);

		reply = vfdw_batch_next(batch);
		vfdw_reply_expect(reply, VFDW_RTYPE(VALKEY_REPLY_STRING) |
						  VFDW_RTYPE(VALKEY_REPLY_STATUS),
						  "could not ask a cluster node for its id");

		values[0] = CStringGetTextDatum(map->nodes[i].host);
		values[1] = Int32GetDatum(map->nodes[i].port);
		values[2] = BoolGetDatum(true);
		values[3] = PointerGetDatum(cstring_to_text_with_len(reply->str,
															 reply->len));

		vfdw_batch_end(batch);
		vfdw_release_connection(vconn);
		MemoryContextDelete(mctx);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}
