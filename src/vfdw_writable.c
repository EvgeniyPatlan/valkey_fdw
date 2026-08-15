/*-------------------------------------------------------------------------
 *
 * vfdw_writable.c
 *		Which commands a foreign table accepts, and why it accepts no more.
 *
 * Split from src/vfdw_map.c, which resolves a table's shape by building its
 * map. This file must answer without building one and without raising, so it
 * parses the same options a second time with parsers that report failure -
 * a duplication the comment on vfdw_map_writability explains and which the
 * file-length gate makes visible rather than buried.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_writable.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_map.h"

#include "access/htup_details.h"
#include "catalog/pg_attribute.h"
#include "commands/defrem.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

/*
 * Which commands this table could accept, without building its map.
 *
 * IsForeignRelUpdatable is called by the rewriter for every foreign table it
 * touches, and by information_schema.tables for every relation in the
 * database. It must therefore never raise: vfdw_map_build refuses a table
 * whose column reads ttl or distance, and the regression database is left
 * holding several of those, so a map built here would break the next suite's
 * first look at information_schema rather than the write it was asked about.
 *
 * So the options are read directly, with parsers that report failure instead
 * of raising. They were validated at DDL time; anything unrecognised here is
 * treated as a table this wrapper will not write to, which is the safe answer.
 */
typedef struct VfdwWritability
{
	const char *tabletype;
	bool		readonly;
	bool		legacy;
	bool		search_index;
	int			keyopts;		/* how many of the three key options appear */
} VfdwWritability;

static void
vfdw_map_read_writability(List *options, VfdwWritability *w)
{
	ListCell   *lc;

	w->tabletype = "string";
	w->readonly = false;
	w->legacy = false;
	w->search_index = false;
	w->keyopts = 0;

	foreach(lc, options)
	{
		DefElem    *elem = (DefElem *) lfirst(lc);
		const char *name = elem->defname;
		char	   *value = defGetString(elem);
		bool		flag;

		if (strcmp(name, "tabletype") == 0)
			w->tabletype = value;
		else if (strcmp(name, "search_index") == 0)
			w->search_index = true;
		else if (strcmp(name, "readonly") == 0)
			w->readonly = w->readonly || (parse_bool(value, &flag) && flag);
		else if (strcmp(name, "legacy_value") == 0)
			w->legacy = w->legacy || (parse_bool(value, &flag) && flag);
		else if (strcmp(name, "singleton_key") == 0 ||
				 strcmp(name, "keyprefix") == 0 ||
				 strcmp(name, "keyset") == 0)
			w->keyopts++;
	}
}

/*
 * Does any column of this relation name a source nothing fills yet?
 *
 * Table options alone used to decide writability, so a hash table with a ttl
 * column was advertised through information_schema as fully writable while it
 * could not even be SELECTed. The syscache is walked directly rather than
 * through the relcache because this must not open a relation and must not
 * raise; attnums are contiguous, so the first miss is the end.
 */
static bool
vfdw_map_has_unimplemented_column(Oid relid)
{
	AttrNumber	attnum;

	for (attnum = 1; attnum <= MaxHeapAttributeNumber; attnum++)
	{
		HeapTuple	tp;
		ListCell   *lc;

		tp = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid),
							 Int16GetDatum(attnum));
		if (!HeapTupleIsValid(tp))
			return false;
		ReleaseSysCache(tp);

		foreach(lc, GetForeignColumnOptions(relid, attnum))
		{
			DefElem    *elem = (DefElem *) lfirst(lc);
			bool		flag;

			if (strcmp(elem->defname, "ttl") != 0 &&
				strcmp(elem->defname, "distance") != 0)
				continue;
			if (parse_bool(defGetString(elem), &flag) && flag)
				return true;
		}
	}

	return false;
}

/*
 * Why this table accepts no write of any kind, or NULL if it accepts some.
 *
 * The reason travels with the mask because the caller has to put it in an
 * errdetail, and a single hardcoded errdetail is what made a search_index
 * table refuse an INSERT with a paragraph about list row identity.
 */
static const char *
vfdw_map_write_block(Oid relid, const VfdwWritability *w, const char **hint)
{
	if (w->readonly)
	{
		*hint = "Remove OPTIONS (readonly 'true') to allow writes.";
		return "The table is declared read-only.";
	}
	if (w->legacy)
	{
		/*
		 * Read-only, and not merely unimplemented: an array of members carries
		 * no identity for any member in it, so nothing in a packed row says
		 * WHICH member an UPDATE changed or whether a shorter array means
		 * "remove the rest" or "leave them alone". The mapped shapes answer
		 * that by naming a member or a field per column, which is where a
		 * write belongs.
		 */
		*hint = "Map the members to columns of their own to write them.";
		return "A legacy_value table reads the whole collection into one "
			"array, and an array of members says nothing about which member a "
			"write means.";
	}
	if (w->search_index)
	{
		*hint = "Drop the search_index option to use the ordinary key path.";
		return "A table with a search_index is served by the search phase, "
			"which is not implemented.";
	}
	if (w->keyopts > 1)
	{
		*hint = "Keep exactly one of singleton_key, keyprefix and keyset.";
		return "This table names more than one key-discovery option, so no "
			"key can be resolved for a row.";
	}
	if (vfdw_map_has_unimplemented_column(relid))
	{
		*hint = "Drop the column, or wait for the phase that fills it.";
		return "A column of this table reads ttl or distance, neither of "
			"which is implemented.";
	}

	return NULL;
}

int
vfdw_map_writability(Oid relid, const char **detail, const char **hint)
{
	ForeignTable *table = GetForeignTable(relid);
	VfdwWritability w;
	const char *tip = NULL;
	const char *why;
	int			mask;

	vfdw_map_read_writability(table->options, &w);

	why = vfdw_map_write_block(relid, &w, &tip);
	if (why != NULL)
		mask = 0;
	else if (strcmp(w.tabletype, "list") == 0)
	{
		/*
		 * A list row has no identity that survives its neighbours moving:
		 * LREM removes by value and a concurrent push shifts every position,
		 * so there is nothing an UPDATE could name. Rows can be added and
		 * removed by value, so this is a partial mask with a reason rather
		 * than a refusal.
		 */
		why = "A list row has no identity that survives its neighbours "
			"moving: members are removed by value, and a concurrent push "
			"shifts every position.";
		tip = "DELETE the row and INSERT its replacement.";
		mask = (1 << CMD_INSERT) | (1 << CMD_DELETE);
	}
	else
		mask = (1 << CMD_INSERT) | (1 << CMD_UPDATE) | (1 << CMD_DELETE);

	if (detail != NULL)
		*detail = why;
	if (hint != NULL)
		*hint = tip;

	return mask;
}

int
vfdw_map_writability_for_relid(Oid relid)
{
	return vfdw_map_writability(relid, NULL, NULL);
}
