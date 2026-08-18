/*-------------------------------------------------------------------------
 *
 * vfdw_estimate.c
 *		How many rows a table has, when the server can say in one command.
 *
 * Every table plans at a placeholder until someone runs ANALYZE, and
 * autovacuum never analyzes a foreign table - so a three-member zset and a
 * two-million-member one produce the same plan, for as long as nobody
 * remembers to measure them.
 *
 * TWO SHAPES CAN BE ASKED CHEAPLY, and only two. A singleton_key table is one
 * key, so its size is one LLEN, SCARD, ZCARD or HLEN. A keyset table's keys
 * are the members of a set, so their number is one SCARD. Everything else is a
 * keyspace, and counting a keyspace means scanning it: a DBSIZE per plan is a
 * bad trade at any size, and a MATCH count is worse. Those keep the
 * placeholder, and the README says ANALYZE is the way to improve them.
 *
 * SIZE IS NOT ROW COUNT, which is the part worth stating separately. A hash of
 * fifty fields mapped one column per field is ONE row; the same hash read as a
 * packed collection is also one row; a list of fifty members read one member
 * per row is fifty. vfdw_scan_is_multirow already draws that line for the
 * executor, and it is the same line here - asking the server for a number and
 * reporting it as the row count regardless is how an estimate becomes
 * confidently wrong rather than merely absent.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_estimate.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_estimate.h"

#include "foreign/foreign.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "vfdw_cmd.h"
#include "vfdw_conn.h"
#include "vfdw_row.h"

/*
 * The command that counts one key's members, by table type, or NULL for a
 * string - which holds one value and needs no asking.
 */
static const char *
vfdw_estimate_count_verb(VfdwTableType type)
{
	switch (type)
	{
		case VFDW_TABLE_HASH:
			return "HLEN";
		case VFDW_TABLE_LIST:
			return "LLEN";
		case VFDW_TABLE_SET:
			return "SCARD";
		case VFDW_TABLE_ZSET:
			return "ZCARD";
		case VFDW_TABLE_STRING:
		case VFDW_TABLE_VECTOR:

			/*
			 * A string holds one value, and a vector table is an index rather
			 * than a key: FT.SEARCH answers how many documents matched a
			 * query, which is not the same question as how many rows the
			 * table has. Neither has a count worth one command.
			 */
			break;
	}
	return NULL;
}

/*
 * Ask the server for one count, or return -1.
 *
 * FAILS SOFT, deliberately and in every direction: an unreachable server, a
 * refused command, a reply that is not an integer. This runs during planning,
 * and the alternative to an estimate is the placeholder every table used
 * before - whereas raising here would turn a server that is merely down into a
 * planning error for statements that might never have executed. A scan against
 * that server fails at execution, which is where it belongs and where the
 * message can say what was being read.
 */
static int64
vfdw_estimate_ask(Oid relid, const char *verb, const char *arg, size_t arglen)
{
	ForeignTable *table;
	ForeignServer *server;
	UserMapping *user;
	VfdwConn   *vconn;
	VfdwBatch  *batch;
	VfdwCmd		cmd;
	valkeyReply *reply;
	volatile int64 count = -1;
	MemoryContext cxt;

	table = GetForeignTable(relid);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(GetUserId(), table->serverid);

	cxt = AllocSetContextCreate(CurrentMemoryContext, "valkey_fdw estimate",
								ALLOCSET_SMALL_SIZES);

	PG_TRY();
	{
		vconn = vfdw_get_connection_cluster(server, user);
		batch = vfdw_batch_begin(vconn, cxt);

		vfdw_cmd_init(&cmd, cxt, 3);
		vfdw_cmd_add_cstr(&cmd, verb);
		vfdw_cmd_add_bytes(&cmd, arg, arglen);
		vfdw_batch_add(batch, &cmd);

		reply = vfdw_batch_next(batch);
		if (reply != NULL && reply->type == VALKEY_REPLY_INTEGER)
			count = reply->integer;

		vfdw_batch_end(batch);
		vfdw_release_connection(vconn);
	}
	PG_CATCH();
	{
		/*
		 * Swallowed on purpose; see the note above. FlushErrorState is what
		 * makes this a non-event rather than an error the next statement
		 * inherits.
		 */
		count = -1;
		FlushErrorState();
	}
	PG_END_TRY();

	MemoryContextDelete(cxt);
	return (int64) count;
}

double
vfdw_estimate_rows(Oid relid, const VfdwTableMap *map)
{
	const char *verb;

	/*
	 * A singleton_key table is one key. Its row count is that key's member
	 * count only when the table produces a row per member; mapped columns and
	 * a packed collection both make it exactly one row, which needs no server
	 * to work out.
	 */
	if (map->singleton_key != NULL)
	{
		if (!vfdw_scan_is_multirow(map))
			return 1.0;

		verb = vfdw_estimate_count_verb(map->tabletype);
		if (verb == NULL)
			return 1.0;

		{
			int64		n = vfdw_estimate_ask(relid, verb, map->singleton_key,
											  strlen(map->singleton_key));

			return n >= 0 ? (double) n : -1.0;
		}
	}

	/*
	 * A keyset table's keys are the members of one set, so SCARD counts them -
	 * and counts ROWS only where each key is one row. Where a key is a
	 * collection read member by member, the number of rows is the sum of the
	 * collections' sizes, which is not one command and is not worth several:
	 * the placeholder stands and ANALYZE remains the answer.
	 */
	if (map->keyset != NULL && !vfdw_scan_is_multirow(map))
	{
		int64		n = vfdw_estimate_ask(relid, "SCARD", map->keyset,
										  strlen(map->keyset));

		return n >= 0 ? (double) n : -1.0;
	}

	return -1.0;
}
