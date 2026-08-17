/*-------------------------------------------------------------------------
 *
 * vfdw_map_check.c
 *		Which column kinds a table type admits, and which it cannot.
 *
 * SEPARATE FROM vfdw_map.c because these are rules rather than resolution.
 * vfdw_map.c reads a catalogue row and works out where each column's value
 * comes from; this file answers a different question about the result - given
 * that shape, is it a shape at all. The two change for different reasons: the
 * first when a new option is read, the second when a new kind is admitted or
 * an existing one is admitted somewhere new.
 *
 * It is also where the list grows. Every column kind added to this wrapper
 * adds an arm here and nowhere else, so keeping them together is what makes
 * "which types is this legal for" a question with one place to look.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_map_check.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_map.h"

#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "utils/lsyscache.h"

#include "vfdw_map_check.h"

/*
 * Why this column cannot mean anything for this table type, or NULL.
 *
 * Separate from the walk that reports it so the rules read as a list of rules.
 * It also assigns the two per-kind counters, which belong to the one pass that
 * visits every column in order: a ttl column's slot is its position among the
 * ttl columns, and deciding that anywhere else would be a second walk to keep
 * in step with this one.
 */
static const char *
vfdw_map_column_conflict(VfdwTableMap *map, VfdwColumn *col)
{
	switch (col->kind)
	{
		case VFDW_COL_FIELD:
			map->nfields++;
			if (map->tabletype != VFDW_TABLE_HASH)
				return "field columns require tabletype 'hash'";
			return NULL;

		case VFDW_COL_SCORE:
			if (map->tabletype != VFDW_TABLE_ZSET)
				return "score columns require tabletype 'zset'";
			return NULL;

		case VFDW_COL_MEMBER:
			if (map->tabletype != VFDW_TABLE_LIST &&
				map->tabletype != VFDW_TABLE_SET &&
				map->tabletype != VFDW_TABLE_ZSET)
				return "member columns require tabletype 'list', 'set' or 'zset'";
			return NULL;

		case VFDW_COL_POSITION:

			/*
			 * A list, and nothing else. A set has no order at all, and a zset's
			 * order is its scores - which a score column already reports, and
			 * which survives a concurrent write in a way an index does not.
			 */
			if (map->tabletype != VFDW_TABLE_LIST)
				return "position columns require tabletype 'list'";
			if (getBaseType(col->typid) != INT4OID &&
				getBaseType(col->typid) != INT8OID)
				return "position columns must be of type integer or bigint";
			return NULL;

		case VFDW_COL_TTL:
			col->ttl_slot = map->nttl++;
			if (map->tabletype != VFDW_TABLE_HASH)
				return "ttl columns require tabletype 'hash'";
			if (col->field == NULL)
				return "a ttl column must also name its field";
			if (getBaseType(col->typid) != INTERVALOID)
				return "ttl columns must be of type interval";
			return NULL;

		case VFDW_COL_DISTANCE:
			if (map->search_index == NULL)
				return "distance columns require the search_index option";
			return NULL;

		case VFDW_COL_VALUE:
			if (map->tabletype != VFDW_TABLE_STRING)
				return "this table type has no single value column";
			return NULL;

		default:
			return NULL;
	}
}

/*
 * Reject column kinds that cannot mean anything for this table type.
 */
void
vfdw_map_check_types(VfdwTableMap *map, TupleDesc tupdesc)
{
	int			i;

	for (i = 0; i < map->natts; i++)
	{
		const char *why = vfdw_map_column_conflict(map, &map->cols[i]);

		if (why != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("column \"%s\" cannot be used with tabletype \"%s\"",
							NameStr(TupleDescAttr(tupdesc, i)->attname),
							vfdw_tabletype_name(map->tabletype)),
					 errdetail("%s.", why)));
	}
}

/*
 * Refuse what the option grammar accepts but the scan cannot yet read.
 *
 * These shapes are part of the design and their options validate, but nothing
 * fills their columns yet. Leaving them to return NULL would be a plausible
 * empty result - the failure mode this wrapper exists to avoid - so they are
 * refused at plan time until the phase that implements them lands. Checked
 * after validity, so a table that is both malformed and unimplemented is
 * reported as malformed, which is the more useful of the two.
 */
void
vfdw_map_check_implemented(VfdwTableMap *map, TupleDesc tupdesc)
{
	int			i;

	for (i = 0; i < map->natts; i++)
	{
		const char *what = NULL;

		/*
		 * ttl has left this list. It is read, by a second command per key, and
		 * what it needs from the server is asked of the server rather than
		 * decided here - see vfdw_ttl.h. A server without per-field expiry is
		 * refused when the scan opens, which is the first moment there is a
		 * server to ask.
		 */
		if (map->cols[i].kind == VFDW_COL_DISTANCE)
			what = "distance";

		if (what == NULL)
			continue;

		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("column \"%s\" reads %s, which is not implemented yet",
						NameStr(TupleDescAttr(tupdesc, i)->attname), what)));
	}
}

/*
 * Decide once whether this table's hash reads are HMGET, and assign each field
 * column its position in that reply.
 *
 * ONE WALK, in map order, so the order fields are ASKED for and the order
 * their answers are READ from are the same order by construction - the same
 * reason a ttl column's slot is assigned where the ttl columns are counted.
 *
 * A ttl column's field is asked for too, even though its expiry comes from
 * HPTTL and its value is never read. That is what keeps "does this key hold
 * any of our fields" answerable: HMGET replies with one entry per field asked
 * for and never with an empty array, so a key that is gone and a key holding
 * none of the fields both answer all-nil - and a ttl-only field left out of
 * the question would make a key that holds only that field look like neither.
 * Duplicates are left in rather than merged: a field named by both a value
 * column and a ttl column costs one repeated argument, and merging them would
 * be a second thing to keep in step with the positions.
 */
static void
vfdw_map_plan_hash_fetch(VfdwTableMap *map)
{
	int			i;

	map->hmget = false;
	map->nreq = 0;

	if (map->tabletype != VFDW_TABLE_HASH || map->legacy_value)
		return;

	for (i = 0; i < map->natts; i++)
	{
		VfdwColumn *col = &map->cols[i];

		if (col->kind == VFDW_COL_FIELD)
			col->field_slot = map->nreq++;
		else if (col->kind == VFDW_COL_TTL)
			map->nreq++;
	}

	/* HMGET with no fields is not a command. */
	map->hmget = map->nreq > 0;
}

void
vfdw_map_check_fetch(VfdwTableMap *map)
{
	vfdw_map_plan_hash_fetch(map);
}
