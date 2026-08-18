/*-------------------------------------------------------------------------
 *
 * vfdw_map.c
 *		Foreign table shape resolution and validation.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_map.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_map.h"

#include "access/table.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "lib/stringinfo.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "vfdw_map_check.h"

static const char *const vfdw_tabletype_names[] = {
	"string", "hash", "list", "set", "zset", "vector"
};

/*
 * The bound is this array's own length rather than the last member of the
 * enum. The two are edited for different reasons and at different times, and
 * an enum that gains a member before the names do would index past the end of
 * the array and hand an error message whatever bytes follow it.
 */
const char *
vfdw_tabletype_name(VfdwTableType type)
{
	if (type < 0 || type >= (int) lengthof(vfdw_tabletype_names))
		return "unknown";
	return vfdw_tabletype_names[type];
}

/*
 * The one meaning a table of this type leaves for a column it was not told
 * about, or VFDW_COL_DROPPED when it leaves more than one.
 *
 * A zset's remaining columns could be its member or its score, and a hash's
 * could be any field, so those imply nothing and have to be declared. Used
 * both to assign defaults and to explain a refusal, so the rule and its
 * explanation cannot drift apart.
 */
static VfdwColKind
vfdw_map_default_kind(VfdwTableType type)
{
	switch (type)
	{
		case VFDW_TABLE_STRING:
			return VFDW_COL_VALUE;
		case VFDW_TABLE_LIST:
		case VFDW_TABLE_SET:
			return VFDW_COL_MEMBER;
		default:
			return VFDW_COL_DROPPED;
	}
}

static const char *
vfdw_tabletype_supplies(VfdwTableType type)
{
	switch (type)
	{
		case VFDW_TABLE_STRING:
			return "a key and one value";
		case VFDW_TABLE_HASH:
			return "a key and named fields";
		case VFDW_TABLE_LIST:
		case VFDW_TABLE_SET:
			return "a key and one member";
		case VFDW_TABLE_ZSET:
			return "a key, a member and a score";
		case VFDW_TABLE_VECTOR:
			return "a key, named fields and a distance";
	}

	/*
	 * No default arm, for the reason vfdw_rowid_shape gives: a table type
	 * added to the enum and not to this switch is a compiler warning here and
	 * a wrong sentence in a user's error message otherwise.
	 */
	return "a key";
}

/*
 * A column names exactly one source. Silently preferring one over another
 * would make a typo look like it worked.
 */
static void
vfdw_map_check_single_source(bool is_key, bool is_member, bool is_score,
							 bool is_distance, bool is_ttl, bool is_position,
							 const char *field)
{
	int			claims = (is_key ? 1 : 0) + (is_member ? 1 : 0) +
		(is_score ? 1 : 0) + (is_distance ? 1 : 0) +
		(is_ttl ? 1 : 0) + (is_position ? 1 : 0) +
		(field != NULL && !is_ttl ? 1 : 0);

	if (claims > 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("column options conflict"),
				 errdetail("A column may name only one source: key, field, "
						   "member, score, ttl or distance.")));
}

/*
 * The index_type option as an enum.
 *
 * Spelled out rather than taken as the validator's ordinal, which would be
 * one line: the ordinal is a position in an array in another file, and the
 * two orderings would then have to be kept in step by nobody in particular.
 * Getting that wrong does not fail - it calls a tag column a vector one.
 *
 * vfdw_parse_enum still runs, so the accepted set is still the option table's
 * and this chain cannot widen it. It is a mapping, not a second validator,
 * and the last branch is unreachable by construction.
 */
static VfdwIndexType
vfdw_map_index_type(const VfdwOptionDef *def, const char *value)
{
	(void) vfdw_parse_enum(def, value);

	if (strcmp(value, "tag") == 0)
		return VFDW_INDEX_TAG;
	if (strcmp(value, "numeric") == 0)
		return VFDW_INDEX_NUMERIC;
	if (strcmp(value, "vector") == 0)
		return VFDW_INDEX_VECTOR;

	elog(ERROR, "valkey_fdw: index_type \"%s\" passed the validator and is "
		 "not one of the three", value);
}

/*
 * Read the per-column options into a partially-filled VfdwColumn.
 *
 * Only records what was asked for; whether the request makes sense for this
 * table type is decided afterwards, when the table type is known.
 */
static void
vfdw_map_read_column_options(VfdwColumn *col, List *options)
{
	ListCell   *lc;
	bool		is_key = false;
	bool		is_member = false;
	bool		is_score = false;
	bool		is_ttl = false;
	bool		is_position = false;
	bool		is_distance = false;
	const char *field = NULL;

	foreach(lc, options)
	{
		DefElem    *elem = (DefElem *) lfirst(lc);
		const VfdwOptionDef *def;
		char	   *value;

		def = vfdw_find_option(elem->defname, AttributeRelationId);
		if (def == NULL)
			continue;
		value = defGetString(elem);

		if (strcmp(def->name, "key") == 0)
			is_key = vfdw_parse_bool(def, value);
		else if (strcmp(def->name, "field") == 0)
			field = value;
		else if (strcmp(def->name, "member") == 0)
			is_member = vfdw_parse_bool(def, value);
		else if (strcmp(def->name, "score") == 0)
			is_score = vfdw_parse_bool(def, value);
		else if (strcmp(def->name, "ttl") == 0)
			is_ttl = vfdw_parse_bool(def, value);
		else if (strcmp(def->name, "position") == 0)
			is_position = vfdw_parse_bool(def, value);
		else if (strcmp(def->name, "distance") == 0)
			is_distance = vfdw_parse_bool(def, value);
		else if (strcmp(def->name, "index_type") == 0)
			col->index_type = vfdw_map_index_type(def, value);
	}

	col->field = field;
	vfdw_map_check_single_source(is_key, is_member, is_score, is_distance,
								 is_ttl, is_position, field);

	if (is_key)
		col->kind = VFDW_COL_KEY;
	else if (is_ttl)
		col->kind = VFDW_COL_TTL;
	else if (is_position)
		col->kind = VFDW_COL_POSITION;
	else if (is_member)
		col->kind = VFDW_COL_MEMBER;
	else if (is_score)
		col->kind = VFDW_COL_SCORE;
	else if (is_distance)
		col->kind = VFDW_COL_DISTANCE;
	else if (field != NULL)
		col->kind = VFDW_COL_FIELD;
	else
		col->kind = VFDW_COL_DROPPED;	/* provisional: "unclaimed" */
}

/*
 * Table options that contradict each other.
 *
 * The three key-discovery strategies are alternatives. A rule that is
 * documented but not enforced, because the branch meant to check it is
 * unreachable, leaves the combination accepted and one option silently
 * ignored at runtime.
 *
 * valkey_fdw_validator sees a table's whole option list, so it refuses that
 * combination at CREATE and at ALTER too - which is where the mistake is, and
 * which is what keeps vfdw_map_writability from calling such a table fully
 * updatable. The check here is the second line, for a catalog row nothing
 * validated.
 *
 * The legacy_value rule below has only this line. The validator does not check
 * it, so that contradiction is caught at the first plan rather than at CREATE;
 * it belongs beside the trio in vfdw_check_key_options, which is the one place
 * that sees a table's options together.
 */
static void
vfdw_map_check_table_options(const VfdwTableMap *map)
{
	if (map->singleton_key != NULL && map->keyprefix != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("singleton_key cannot be combined with keyprefix")));
	if (map->singleton_key != NULL && map->keyset != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("singleton_key cannot be combined with keyset")));
	if (map->keyprefix != NULL && map->keyset != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("keyprefix cannot be combined with keyset")));

	/*
	 * legacy_value packs a collection into one array. A "string" key holds one
	 * value and has no members, so the two contradict each other, and the
	 * contradiction is between two TABLE options - which is why it is refused
	 * here beside the three above rather than argued about per column.
	 *
	 * An array of one element is the other defensible answer and is rejected
	 * deliberately: it is the only case in which the array's length would say
	 * nothing about the data, it makes every query unwrap a value that
	 * (key, value text) already delivers, and it would give a string table a
	 * second, differently-shaped answer to "is this key absent" - GET says so
	 * with a nil, while vfdw_scan_reply_is_absent reads emptiness only for the
	 * container types.
	 */
	/*
	 * A vector table is its index. Without one there is nothing to search and
	 * nothing to fall back to: the table names no keyspace of its own, so the
	 * alternative to refusing is a table that can only ever answer nothing.
	 *
	 * Like the trio above, this rule is between two TABLE options, so
	 * vfdw_check_shape_options refuses it at CREATE and at ALTER, where the
	 * mistake is. This is the second line, for a catalog row nothing
	 * validated.
	 */
	if (map->tabletype == VFDW_TABLE_VECTOR && map->search_index == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("tabletype \"vector\" requires the search_index option"),
				 errdetail("A vector table draws its rows from a valkey-search "
						   "index, which nothing here names.")));

	if (map->legacy_value && map->tabletype == VFDW_TABLE_STRING)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("legacy_value requires a collection tabletype"),
				 errdetail("A \"string\" key holds one value, not a collection, "
						   "so there is nothing to pack into an array."),
				 errhint("Use tabletype 'hash', 'list', 'set' or 'zset', or "
						 "drop legacy_value and map the value to a column.")));
}

/*
 * Table-level options.
 */
static void
vfdw_map_read_table_options(VfdwTableMap *map, List *options)
{
	ListCell   *lc;

	map->tabletype = VFDW_TABLE_STRING;
	map->database = 0;

	foreach(lc, options)
	{
		DefElem    *elem = (DefElem *) lfirst(lc);
		const VfdwOptionDef *def;
		char	   *value;

		def = vfdw_find_option(elem->defname, ForeignTableRelationId);
		if (def == NULL)
			continue;
		value = defGetString(elem);

		if (strcmp(def->name, "tabletype") == 0)
			map->tabletype = (VfdwTableType) vfdw_parse_enum(def, value);
		else if (strcmp(def->name, "keyprefix") == 0)
			map->keyprefix = value;
		else if (strcmp(def->name, "keyset") == 0)
			map->keyset = value;
		else if (strcmp(def->name, "singleton_key") == 0)
			map->singleton_key = value;
		else if (strcmp(def->name, "search_index") == 0)
			map->search_index = value;
		else if (strcmp(def->name, "database") == 0)
			map->database = vfdw_parse_int(def, value);
		else if (strcmp(def->name, "legacy_value") == 0)
			map->legacy_value = vfdw_parse_bool(def, value);
		else if (strcmp(def->name, "readonly") == 0)
			map->readonly = vfdw_parse_bool(def, value);
	}

	vfdw_map_check_table_options(map);
}

/*
 * Locate the declared key column and count the columns still unaccounted for.
 */
static int
vfdw_map_survey(VfdwTableMap *map, TupleDesc tupdesc,
				AttrNumber *first_unclaimed)
{
	int			unclaimed = 0;
	int			i;

	*first_unclaimed = InvalidAttrNumber;

	for (i = 0; i < map->natts; i++)
	{
		VfdwColumn *col = &map->cols[i];

		if (col->kind == VFDW_COL_KEY)
		{
			if (map->keyattno != InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("only one column may be the Valkey key"),
						 errdetail("Both \"%s\" and \"%s\" declare key 'true'.",
								   NameStr(TupleDescAttr(tupdesc, map->keyattno - 1)->attname),
								   NameStr(TupleDescAttr(tupdesc, i)->attname))));
			map->keyattno = col->attnum;
		}
		else if (col->kind == VFDW_COL_DROPPED && col->attnum != InvalidAttrNumber)
		{
			unclaimed++;
			if (*first_unclaimed == InvalidAttrNumber)
				*first_unclaimed = col->attnum;
		}
	}

	return unclaimed;
}

/*
 * Report every column that resolved to nothing.
 */
static void
vfdw_map_report_unsourced(VfdwTableMap *map, TupleDesc tupdesc, int unclaimed)
{
	StringInfoData names;
	bool		first = true;
	int			i;

	/*
	 * Name every unsourced column rather than the first one found. With two
	 * spare columns on a string table it is not the first that is wrong - it
	 * is that the table does not say which of them is the value, and blaming
	 * one of them sends the user to the wrong line.
	 */
	initStringInfo(&names);
	for (i = 0; i < map->natts; i++)
	{
		if (map->cols[i].kind != VFDW_COL_DROPPED ||
			map->cols[i].attnum == InvalidAttrNumber)
			continue;
		appendStringInfo(&names, "%s\"%s\"", first ? "" : ", ",
						 NameStr(TupleDescAttr(tupdesc, i)->attname));
		first = false;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
			 errmsg("%d column(s) have no source in Valkey: %s",
					unclaimed, names.data),
			 errdetail("Table type is \"%s\", which supplies %s.",
					   vfdw_tabletype_name(map->tabletype),
					   vfdw_tabletype_supplies(map->tabletype)),
			 errhint("Give each remaining column OPTIONS (field '...'), or "
					 "one of member, score, ttl, distance.")));
}

/*
 * The packed layout: (key, value) by position, no column options.
 *
 * A separate shape rather than a special case threaded through the normal one,
 * because it answers a different question. Every other shape names each hash
 * field in a column option, so a keyspace of per-tenant hashes whose field
 * sets differ from key to key - or are simply not known when the table is
 * defined - has no expressible mapping at all. This one hands the whole
 * collection over in one array and lets the query decide what it is:
 *
 *		SELECT key, (populate_record(NULL::t, hstore(value))).* FROM tbl
 *
 * is schema-on-read, and it is why the array a hash produces is the reply's
 * own field,value,field,value order (see vfdw_row_store_packed).
 */
static void
vfdw_map_assign_legacy(VfdwTableMap *map)
{
	if (map->natts != 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("legacy_value tables must have exactly two columns"),
				 errdetail("This table has %d.", map->natts)));

	/*
	 * A dropped column keeps its attnum, so a table can pass the width test
	 * with only one live column. Refused rather than mapped: the dropped one
	 * has no type resolved behind it, and asking for the element type of a
	 * type that was never looked up is a cache lookup on InvalidOid.
	 */
	if (map->cols[0].attnum == InvalidAttrNumber ||
		map->cols[1].attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("legacy_value tables must have exactly two columns"),
				 errdetail("One of this table's two columns has been dropped.")));

	map->cols[0].kind = VFDW_COL_KEY;
	map->keyattno = map->cols[0].attnum;
	map->cols[1].kind = VFDW_COL_LEGACY_VALUE;
}

/*
 * Claim the column that carries the key, and report how many are left.
 *
 * With no explicit key column the first unclaimed one is the key - the
 * conventional shape, and what makes a two-column table work with no options
 * at all.
 */
static int
vfdw_map_assign_key(VfdwTableMap *map, int unclaimed, AttrNumber first_unclaimed)
{
	if (map->keyattno == InvalidAttrNumber && unclaimed > 0)
	{
		map->cols[first_unclaimed - 1].kind = VFDW_COL_KEY;
		map->keyattno = first_unclaimed;
		unclaimed--;
	}

	if (map->keyattno == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("no column holds the Valkey key"),
				 errhint("Mark one column with OPTIONS (key 'true'), or name "
						 "the key with OPTIONS (singleton_key '...').")));

	return unclaimed;
}

/*
 * Give a lone remaining column the only meaning its table type leaves for it.
 */
static int
vfdw_map_assign_remaining(VfdwTableMap *map, int unclaimed)
{
	VfdwColKind kind = vfdw_map_default_kind(map->tabletype);
	int			i;

	if (unclaimed != 1 || kind == VFDW_COL_DROPPED)
		return unclaimed;

	for (i = 0; i < map->natts; i++)
	{
		VfdwColumn *col = &map->cols[i];

		if (col->kind == VFDW_COL_DROPPED && col->attnum != InvalidAttrNumber)
		{
			col->kind = kind;
			return unclaimed - 1;
		}
	}

	return unclaimed;
}

/*
 * Give every attribute a source, or refuse the table.
 *
 * This is what makes a crash on any table that is not two columns wide
 * unreachable: after it returns, cols[i] is defined for every i < natts, so
 * the executor can fill a slot of any width without reading past anything.
 */
static void
vfdw_map_assign_defaults(VfdwTableMap *map, TupleDesc tupdesc)
{
	int			unclaimed;
	AttrNumber	first_unclaimed;

	unclaimed = vfdw_map_survey(map, tupdesc, &first_unclaimed);

	if (map->legacy_value)
	{
		vfdw_map_assign_legacy(map);
		return;
	}

	/*
	 * A singleton table names its one key in the table options, so no column
	 * has to carry it and none is taken by default. A column may still
	 * declare key 'true' and be filled with that fixed name, which is what
	 * lets such a table join against its siblings on the same column.
	 */
	if (map->singleton_key == NULL)
		unclaimed = vfdw_map_assign_key(map, unclaimed, first_unclaimed);

	unclaimed = vfdw_map_assign_remaining(map, unclaimed);

	if (unclaimed > 0)
		vfdw_map_report_unsourced(map, tupdesc, unclaimed);
}



/*
 * Resolve what a packed column's array is made of, or refuse the column.
 *
 * One row carries the whole collection, so the column is an array and its
 * elements are the members. Two element types and no others: text, checked
 * against the server encoding like every other text value, and bytea, which
 * is the only one that can carry a member holding a NUL or bytes that are not
 * valid in the server encoding at all (invariant I3). A column that is neither
 * is a table definition error naming the column, because the alternative -
 * discovering it when the first row is built - is a runtime failure per key
 * for a mistake that is visible in the CREATE statement.
 */
static void
vfdw_map_resolve_packed(VfdwTableMap *map, TupleDesc tupdesc)
{
	VfdwPackedElem *elem;
	VfdwColumn *col;
	const char *name;
	Oid			elemtypid;
	Oid			elembase;

	if (!map->legacy_value)
		return;

	/* vfdw_map_assign_legacy fixed the shape: the key first, the array last. */
	col = &map->cols[map->natts - 1];
	name = NameStr(TupleDescAttr(tupdesc, map->natts - 1)->attname);

	/*
	 * A domain over the ARRAY is refused rather than quietly accepted: the
	 * array is assembled from element Datums and never passes through
	 * array_in, so a constraint on the array as a whole would never run, and a
	 * constraint that never runs is worse than a table that is not created. A
	 * domain over the ELEMENT is fine - vfdw_row_datum_from_bytes applies that
	 * one per element, exactly as it does for a scalar column.
	 */
	if (col->is_domain)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("column \"%s\" cannot be a domain over an array", name),
				 errdetail("The array is built from its elements rather than "
						   "parsed, so a constraint on the array itself would "
						   "never be checked."),
				 errhint("Declare the column text[] or bytea[], or move the "
						 "domain to the element type.")));

	elemtypid = get_element_type(col->typid);
	elembase = OidIsValid(elemtypid) ? getBaseType(elemtypid) : InvalidOid;

	if (elembase != TEXTOID && elembase != BYTEAOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("column \"%s\" must be an array of text or of bytea",
						name),
				 errdetail("A legacy_value table gives one row per key with the "
						   "whole collection in this column, so it holds the "
						   "members rather than one value."),
				 errhint("Use text[] for members that are text in the server "
						 "encoding, or bytea[] for arbitrary bytes.")));

	elem = palloc0(sizeof(VfdwPackedElem));
	vfdw_map_resolve_type(&elem->col, elemtypid, col->typmod);
	elem->col.attnum = col->attnum;
	elem->col.kind = col->kind;
	get_typlenbyvalalign(elemtypid, &elem->typlen, &elem->typbyval,
						 &elem->typalign);
	col->packed = elem;
}


/*
 * Everything a column derives from its declared PostgreSQL type.
 *
 * Both I/O directions are resolved in one place because they are one
 * decision: whichever type the inbound side reads through, the outbound side
 * has to write through, or a value cannot survive a round trip. is_binary is
 * taken from the base type so that a domain over bytea is binary both ways -
 * with atttypid alone it would be non-binary, and its bytes would be pushed
 * through pg_verifymbstr and bytea_in on the way back in.
 */
void
vfdw_map_resolve_type(VfdwColumn *col, Oid typid, int32 typmod)
{
	Oid			basetypid = getBaseType(typid);
	Oid			typinput;
	Oid			typoutput;
	bool		typisvarlena;

	col->typid = typid;
	col->typmod = typmod;
	col->is_binary = (basetypid == BYTEAOID);
	col->is_domain = (basetypid != typid);

	getTypeInputInfo(typid, &typinput, &col->typioparam);
	col->typinput = typinput;

	getTypeOutputInfo(typid, &typoutput, &typisvarlena);
	col->typoutput = typoutput;
	col->typisvarlena = typisvarlena;
}

/*
 * Record the attnum of each role the write path addresses, and refuse a table
 * that names any of them twice.
 *
 * The refusal is not required by the read path, which fills both columns with
 * the same bytes and is merely redundant. It is required by the write path,
 * where two member columns holding different values describe two different
 * rows and no rule chooses between them.
 *
 * Refused here, at the first plan, and NOT at CREATE - a previous version of
 * this comment claimed otherwise. It cannot be at CREATE: valkey_fdw_validator
 * is invoked once per catalog object with that object's own options, so when
 * it runs for a column it sees that column's options and nothing else, and
 * "two columns claim the same role" is not a question a single column's
 * option list can answer. The table-level key options ARE checked in the
 * validator, because there the whole list arrives at once. Moving this there
 * would mean re-reading every sibling column's options from the syscache
 * inside a validator that runs once per column, to answer the same question
 * n times.
 */
static void
vfdw_map_index_roles(VfdwTableMap *map, TupleDesc tupdesc)
{
	int			i;

	map->memberattno = InvalidAttrNumber;
	map->scoreattno = InvalidAttrNumber;
	map->valueattno = InvalidAttrNumber;

	for (i = 0; i < map->natts; i++)
	{
		VfdwColumn *col = &map->cols[i];
		AttrNumber *slot;

		if (col->kind == VFDW_COL_MEMBER)
			slot = &map->memberattno;
		else if (col->kind == VFDW_COL_SCORE)
			slot = &map->scoreattno;
		else if (col->kind == VFDW_COL_VALUE)
			slot = &map->valueattno;
		else
			continue;

		if (*slot != InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("only one column may hold each Valkey role"),
					 errdetail("Both \"%s\" and \"%s\" claim the same role.",
							   NameStr(TupleDescAttr(tupdesc, *slot - 1)->attname),
							   NameStr(TupleDescAttr(tupdesc, i)->attname))));

		*slot = col->attnum;
	}
}

VfdwTableMap *
vfdw_map_build(Relation rel, ForeignTable *table)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	VfdwTableMap *map = palloc0(sizeof(VfdwTableMap));
	int			i;

	map->relid = RelationGetRelid(rel);
	map->natts = tupdesc->natts;
	map->cols = palloc0(sizeof(VfdwColumn) * Max(map->natts, 1));
	map->keyattno = InvalidAttrNumber;

	vfdw_map_read_table_options(map, table->options);

	for (i = 0; i < map->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
		VfdwColumn *col = &map->cols[i];
		List	   *colopts;

		if (attr->attisdropped)
		{
			col->kind = VFDW_COL_DROPPED;
			col->attnum = InvalidAttrNumber;
			continue;
		}

		col->attnum = attr->attnum;
		vfdw_map_resolve_type(col, attr->atttypid, attr->atttypmod);

		colopts = GetForeignColumnOptions(map->relid, attr->attnum);
		vfdw_map_read_column_options(col, colopts);
	}

	vfdw_map_assign_defaults(map, tupdesc);
	vfdw_map_check_types(map, tupdesc);
	vfdw_map_resolve_packed(map, tupdesc);
	vfdw_map_index_roles(map, tupdesc);
	vfdw_map_check_implemented(map, tupdesc);
	vfdw_map_check_fetch(map);

	return map;
}

VfdwTableMap *
vfdw_map_build_for_relid(Oid relid)
{
	Relation	rel = table_open(relid, NoLock);
	ForeignTable *table = GetForeignTable(relid);
	VfdwTableMap *map = vfdw_map_build(rel, table);
	table_close(rel, NoLock);
	return map;
}
