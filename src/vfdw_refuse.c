/*-------------------------------------------------------------------------
 *
 * vfdw_refuse.c
 *		The write path's refusal gates.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_refuse.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_refuse.h"

#include "utils/lsyscache.h"

#include "vfdw_error.h"

void
vfdw_refuse_unwritable(Oid relid, const VfdwTableMap *map, CmdType operation)
{
	const char *detail = NULL;
	const char *hint = NULL;
	int			mask;

	if (map->readonly)
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("foreign table \"%s\" is declared read-only",
						get_rel_name(relid)),
				 errhint("Remove OPTIONS (readonly 'true') to allow writes.")));

	/*
	 * The reason comes back with the mask. A single hardcoded errdetail here
	 * would explain every refusal with the list row-identity paragraph, so a
	 * string table blocked for carrying a search index would be refused for a
	 * reason that had nothing to do with it.
	 */
	mask = vfdw_map_writability(relid, &detail, &hint);
	if (!(mask & (1 << operation)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot %s a Valkey \"%s\" table",
						operation == CMD_INSERT ? "insert into" :
						operation == CMD_DELETE ? "delete from" : "update",
						vfdw_tabletype_name(map->tabletype)),
				 detail != NULL ? errdetail("%s", detail) : 0,
				 hint != NULL ? errhint("%s", hint) : 0));
}

/*
 * ON CONFLICT DO UPDATE is already refused by infer_arbiter_indexes before we
 * are called, so only bare ONCONFLICT_NOTHING reaches here. The reasoning
 * lives in the errhint rather than only in the README because that is where
 * the user is looking when it fires.
 */
void
vfdw_refuse_on_conflict(const ModifyTable *plan)
{
	if (plan->onConflictAction == ONCONFLICT_NONE)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("INSERT ... ON CONFLICT is not supported on Valkey foreign tables"),
			 errdetail("ExecForeignInsert must return a row before the "
					   "pre-commit flush can know the row will be skipped, "
					   "so the reported row count would be wrong in exactly "
					   "the case ON CONFLICT asks about."),
			 errhint("Handle unique_violation instead.")));
}

void
vfdw_refuse_prefer_replica(const ForeignServer *server)
{
	ereport(ERROR,
			(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
			 errmsg("cannot write through a Valkey server declared prefer_replica"),
			 errdetail("Server \"%s\" is configured to prefer a replica, "
					   "which cannot accept writes.", server->servername),
			 errhint("Remove OPTIONS (prefer_replica 'true'), or write "
					 "through a foreign server pointed at the primary.")));
}

void
vfdw_refuse_no_modify_state(void)
{
	/*
	 * Every path that reaches an Exec callback now sets a state:
	 * BeginForeignModify for plain DML, BeginForeignInsert for COPY and tuple
	 * routing. So this is unreachable, and it stays as an elog rather than
	 * being deleted because the alternative to raising here is dereferencing
	 * NULL - and a fourth entry point added later would find this instead of a
	 * crash.
	 */
	elog(ERROR,
		 "valkey_fdw: a modify callback was reached with no state");
}

/*
 * Names the key, not just the table.
 *
 * A unique violation whose message does not say which value collided is the
 * one every DBA has sworn at. The key travels with its length because it is
 * not NUL-terminated, and it may hold arbitrary bytes - a bytea key column is
 * documented to carry them - so vfdw_safe_text decides whether the value can
 * be shown at all (invariant I2) and the checked copy goes through "%s". A
 * length-counted "%.*s" here would put the raw bytes into the server log and
 * on the wire, and where client_encoding differs from the server encoding the
 * conversion would run during error reporting.
 *
 * Parenthesised rather than quoted, in core's Key (col)=(val) spelling,
 * because when the bytes cannot be shown the substitution is itself a phrase
 * naming an encoding, and quotes around it would nest.
 */
void
vfdw_refuse_duplicate_key(Relation rel, const char *key, size_t keylen)
{
	ereport(ERROR,
			(errcode(ERRCODE_UNIQUE_VIOLATION),
			 errmsg("duplicate key value violates the Valkey keyspace"),
			 errdetail("Key (%s) was already created or modified by this "
					   "transaction, so inserting it again would overwrite a "
					   "row this transaction has not committed yet.",
					   vfdw_safe_text(key, keylen)),
			 errhint("valkey_fdw applies a transaction's writes as one unit, "
					 "so a key may be created once per transaction. Use "
					 "UPDATE to change a row this transaction already wrote."),
			 rel != NULL ? errtable(rel) : 0));
}

void
vfdw_refuse_duplicate_member(Relation rel, const char *key, size_t keylen,
								   const char *sub, size_t sublen,
								   const char *what)
{
	/* Both halves of the row's identity are user bytes. See I2 above. */
	ereport(ERROR,
			(errcode(ERRCODE_UNIQUE_VIOLATION),
			 errmsg("duplicate %s violates the Valkey keyspace", what),
			 errdetail("This transaction already added %s (%s) to key (%s).",
					   what, vfdw_safe_text(sub, sublen),
					   vfdw_safe_text(key, keylen)),
			 errhint("A %s may be added once per transaction. Use UPDATE to "
					 "change one this transaction already wrote.", what),
			 rel != NULL ? errtable(rel) : 0));
}
