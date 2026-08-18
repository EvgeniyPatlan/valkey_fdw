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
 * A position column belongs to a list and nothing else. A set has no order at
 * all, and a zset's order is its scores - which a score column already
 * reports, and which survives a concurrent write in a way an index does not.
 */
static const char *
vfdw_map_position_conflict(VfdwTableMap *map, VfdwColumn *col)
{
	if (map->tabletype != VFDW_TABLE_LIST)
		return "position columns require tabletype 'list'";
	if (getBaseType(col->typid) != INT4OID &&
		getBaseType(col->typid) != INT8OID)
		return "position columns must be of type integer or bigint";
	return NULL;
}

/*
 * A ttl column reports one field's lifetime, so it needs a hash, a field to be
 * paired with, and a type that can hold a duration.
 *
 * The slot is claimed before any of that is checked: it is the position this
 * column's answer is read from, and the count HPTTL is told, and the two are
 * assigned by this one walk so they cannot disagree.
 */
static const char *
vfdw_map_ttl_conflict(VfdwTableMap *map, VfdwColumn *col)
{
	col->ttl_slot = map->nttl++;

	if (map->tabletype != VFDW_TABLE_HASH)
		return "ttl columns require tabletype 'hash'";
	if (col->field == NULL)
		return "a ttl column must also name its field";
	if (getBaseType(col->typid) != INTERVALOID)
		return "ttl columns must be of type interval";
	return NULL;
}

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

			/*
			 * A vector table's rows are hash keys too - FT.SEARCH over an
			 * ON HASH index returns their fields - so a field column means
			 * the same thing there as it does on a hash table.
			 */
			if (map->tabletype != VFDW_TABLE_HASH &&
				map->tabletype != VFDW_TABLE_VECTOR)
				return "field columns require tabletype 'hash' or 'vector'";
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
			return vfdw_map_position_conflict(map, col);

		case VFDW_COL_TTL:
			return vfdw_map_ttl_conflict(map, col);

		case VFDW_COL_DISTANCE:

			/*
			 * One rule, not two. "It also needs a search_index" was the
			 * second, and a vector table cannot lack one: that is refused at
			 * CREATE and again by vfdw_map_check_table_options, which runs
			 * before this walk. Keeping it would have been a branch no input
			 * reaches, saying something the table type already guarantees.
			 */
			if (map->tabletype != VFDW_TABLE_VECTOR)
				return "distance columns require tabletype 'vector'";
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
 * EMPTY, and kept rather than deleted. Every shape that used to be listed
 * here now reads: ttl asks the server whether it can, and distance arrives
 * from the search that a tabletype 'vector' table is planned as. The function
 * stays because the RULE it enforces is the one this tree keeps returning to
 * - a column nothing fills must raise rather than return NULL, because a
 * plausible empty result is the failure with no symptom - and the next option
 * accepted ahead of its implementation belongs here on the day it is added,
 * not in a new place someone has to find.
 *
 * Checked after validity, so a table that is both malformed and unimplemented
 * is reported as malformed, which is the more useful of the two.
 */
void
vfdw_map_check_implemented(VfdwTableMap *map, TupleDesc tupdesc)
{
	(void) map;
	(void) tupdesc;
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
