/*
 * vfdw_ttl.c
 *		Per-field time to live: what to ask a server, and whether it can answer.
 *
 * See vfdw_ttl.h for why this is a file of its own.
 */
#include "postgres.h"

#include "datatype/timestamp.h"
#include "utils/timestamp.h"

#include "vfdw_conn_internal.h"
#include "vfdw_error.h"
#include "vfdw_io.h"
#include "vfdw_ttl.h"

/*
 * The server's two negative answers, which are its own numbers and not ours.
 *
 * -1 is a field that is there and has no expiry; -2 is a field, or a key, that
 * is not there at all. Named because a bare -2 in a comparison reads like a
 * sentinel this file invented, and it is not: changing them changes nothing,
 * they are what the far end sends.
 */
#define VFDW_TTL_NO_EXPIRY (-1)
#define VFDW_TTL_ABSENT (-2)

/*
 * Ask, once, whether this server has per-field expiry.
 *
 * COMMAND INFO and not a trial HPTTL against some key. A trial has to name a
 * key, and then the answer depends on what that key happens to be: a key
 * holding a string answers WRONGTYPE on a server that fully supports the
 * feature, which reads as "unsupported" and is not. COMMAND INFO names no key,
 * changes nothing, and is answered by every server this wrapper builds
 * against.
 *
 * Raises NOTHING after the reply is in hand. A libvalkey reply is malloc'd and
 * outside every MemoryContext, so an ereport between here and freeReplyObject
 * would leak it; the refusal therefore belongs to the caller, after this has
 * returned. vfdw_conn_probe_script is shaped the same way for the same reason.
 */
static void
vfdw_ttl_probe(VfdwConn *vconn)
{
	TimestampTz deadline = vfdw_io_deadline(vconn->opts.command_timeout_ms);

	/*
	 * ALL THREE VERBS, because all three are used and a server is not obliged
	 * to have them together merely because one release shipped them together.
	 * Asking about the read verb alone and then sending a write verb would put
	 * the assumption back that this probe exists to remove.
	 */
	const char *argv[] = {"COMMAND", "INFO", "HPTTL", "HPEXPIRE", "HPERSIST"};
	const size_t arglens[] = {7, 4, 5, 8, 8};
	valkeyReply *reply;
	bool		present = false;

	if (valkeyAppendCommandArgv(vconn->conn, 5, argv, arglens) != VALKEY_OK)
		vfdw_error_from_context(vconn->conn,
								"could not ask the server about per-field expiry");

	vfdw_io_flush(vconn->conn, deadline);
	reply = vfdw_io_get_reply(vconn->conn, deadline);

	/*
	 * One entry per command asked about, in the order asked, and the entry for
	 * a command the server does not have is a nil rather than an omission - so
	 * the array's length says nothing and only the elements do. Every one of
	 * them has to be there.
	 */
	if (!vfdw_reply_is_error(reply) &&
		(reply->type == VALKEY_REPLY_ARRAY || reply->type == VALKEY_REPLY_MAP) &&
		reply->elements >= 3)
	{
		size_t		i;

		present = true;
		for (i = 0; i < 3; i++)
		{
			const valkeyReply *entry = reply->element[i];

			if (entry == NULL ||
				(entry->type != VALKEY_REPLY_ARRAY &&
				 entry->type != VALKEY_REPLY_MAP) ||
				entry->elements == 0)
				present = false;
		}
	}

	vconn->field_ttl = present ? VFDW_CAP_PRESENT : VFDW_CAP_ABSENT;
	freeReplyObject(reply);
}

void
vfdw_ttl_require(VfdwConn *vconn)
{
	if (vconn->field_ttl == VFDW_CAP_UNKNOWN)
		vfdw_ttl_probe(vconn);

	if (vconn->field_ttl == VFDW_CAP_ABSENT)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("this Valkey server has no per-field expiry"),
				 errdetail("A ttl column needs HPTTL to read and HPEXPIRE and "
						   "HPERSIST to write, none of which this server has. "
						   "Per-field expiry arrived in Valkey 9."),
				 errhint("Drop the ttl column to use the rest of this table, "
						 "or point the server at Valkey 9 or later.")));
}

void
vfdw_ttl_command(VfdwCmd *cmd, const VfdwTableMap *map,
				 const char *key, size_t keylen)
{
	int			i;

	vfdw_cmd_reset(cmd);
	vfdw_cmd_add_cstr(cmd, "HPTTL");
	vfdw_cmd_add_bytes(cmd, key, keylen);
	vfdw_cmd_add_cstr(cmd, "FIELDS");
	vfdw_cmd_add_int(cmd, map->nttl);

	for (i = 0; i < map->natts; i++)
	{
		const VfdwColumn *col = &map->cols[i];

		if (col->kind != VFDW_COL_TTL)
			continue;

		/*
		 * The field name travels with its length (I3) even though it came from
		 * a catalogue option and could not contain a NUL: the rule is that
		 * Valkey arguments are bytes, and an exception for the ones that
		 * happen to be safe is how the next argument that is not safe gets
		 * written the same way.
		 */
		vfdw_cmd_add_bytes(cmd, col->field, strlen(col->field));
	}
}

void
vfdw_ttl_take(const VfdwTableMap *map, valkeyReply *reply, int64 *out)
{
	int			i;

	/*
	 * ABSENT, not an error, when the reply is not the array of integers this
	 * asked for. HPTTL answers a missing key with an array of -2 rather than
	 * with an error, so the shapes that land here instead are a server that
	 * refused the read - which the value fetch queued behind this one is about
	 * to report with the key in hand - and a WRONGTYPE against a key holding
	 * something other than a hash, which the value fetch reports the same way.
	 * Raising here would report it first and without the key.
	 */
	for (i = 0; i < map->nttl; i++)
		out[i] = VFDW_TTL_ABSENT;

	if (vfdw_reply_is_error(reply) ||
		(reply->type != VALKEY_REPLY_ARRAY && reply->type != VALKEY_REPLY_MAP))
		return;

	for (i = 0; i < map->nttl && (size_t) i < reply->elements; i++)
	{
		const valkeyReply *e = reply->element[i];

		if (e != NULL && e->type == VALKEY_REPLY_INTEGER)
			out[i] = e->integer;
	}
}

bool
vfdw_ttl_datum(int64 ms, Datum *value)
{
	Interval   *iv;

	if (ms == VFDW_TTL_NO_EXPIRY || ms == VFDW_TTL_ABSENT || ms < 0)
		return false;

	/*
	 * Milliseconds into the interval's own microseconds, checked rather than
	 * multiplied: the server's number is bounded by nothing this code controls,
	 * and an overflow here would land as a plausible short duration.
	 */
	if (ms > (PG_INT64_MAX / 1000))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("Valkey reported a time to live too large for interval"),
				 errdetail("The server answered %lld milliseconds.",
						   (long long) ms)));

	iv = (Interval *) palloc0(sizeof(Interval));
	iv->time = ms * 1000;
	*value = IntervalPGetDatum(iv);
	return true;
}
