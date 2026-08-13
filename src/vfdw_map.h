/*-------------------------------------------------------------------------
 *
 * vfdw_map.h
 *		Foreign table shape: which column comes from where.
 *
 * A table shape fixed at (key, value), with tuples built from a two-element
 * C array regardless of how many columns the table actually has, breaks on
 * the third column: BuildTupleFromCStrings reads past the end of that array,
 * so any user able to CREATE FOREIGN TABLE can crash the backend (ledger
 * columns wide).
 *
 * The answer is not a bounds check bolted onto the same design. It is to
 * resolve, once at plan time, a source for every attribute of the relation -
 * and to refuse the table outright if any attribute has none. The executor
 * then fills a natts-sized slot from a mapping that is known complete
 * (invariant I4).
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_map.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_MAP_H
#define VFDW_MAP_H

#include "vfdw.h"

#include "access/tupdesc.h"
#include "foreign/foreign.h"
#include "utils/relcache.h"

#include "vfdw_option.h"

typedef enum VfdwTableType
{
	VFDW_TABLE_STRING = 0,
	VFDW_TABLE_HASH,
	VFDW_TABLE_LIST,
	VFDW_TABLE_SET,
	VFDW_TABLE_ZSET
} VfdwTableType;

typedef enum VfdwColKind
{
	VFDW_COL_DROPPED = 0,		/* attisdropped; never filled */
	VFDW_COL_KEY,				/* the Valkey key name */
	VFDW_COL_VALUE,				/* the whole value, for a string table */
	VFDW_COL_FIELD,				/* one hash field, named by the field option */
	VFDW_COL_MEMBER,			/* a list, set or zset member */
	VFDW_COL_SCORE,				/* a zset score */
	VFDW_COL_TTL,				/* time to live of the paired field */
	VFDW_COL_DISTANCE,			/* vector search score */
	VFDW_COL_LEGACY_VALUE		/* legacy text[] of the whole value */
} VfdwColKind;

typedef struct VfdwColumn
{
	AttrNumber	attnum;
	VfdwColKind kind;
	const char *field;			/* VFDW_COL_FIELD / VFDW_COL_TTL */
	Oid			typid;
	int32		typmod;

	/* Inbound: bytes -> Datum. */
	Oid			typinput;
	Oid			typioparam;

	/*
	 * Outbound: Datum -> bytes. Resolved beside the inbound pair rather than
	 * at BeginForeignModify so the two directions cannot drift: if the rule
	 * for dropped columns or base types is ever changed for one of them it is
	 * visibly changed for the other.
	 */
	Oid			typoutput;
	bool		typisvarlena;

	/*
	 * bytea columns take Valkey's bytes verbatim. Everything else is a text
	 * representation and must be checked against the server encoding before
	 * it becomes a Datum.
	 *
	 * Computed from the BASE type, so a domain over bytea is binary in both
	 * directions. One expression feeds the read path and the write path,
	 * which is what makes a value written verbatim readable verbatim; two
	 * expressions would eventually disagree and mangle it.
	 */
	bool		is_binary;

	/*
	 * True when typid is a domain. The binary branch bypasses the type's
	 * input function, and with it the domain's constraint check, so the read
	 * path has to apply that check itself.
	 */
	bool		is_domain;
} VfdwColumn;

typedef struct VfdwTableMap
{
	Oid			relid;
	int			natts;
	VfdwColumn *cols;			/* natts entries, indexed by attnum - 1 */

	AttrNumber	keyattno;		/* attnum of the key column, or 0 */

	/*
	 * The other roles the write path addresses directly, resolved once here.
	 * Any of them may be InvalidAttrNumber. The modify callbacks must not
	 * re-derive them by walking cols[]: a walk has to pick a winner when two
	 * columns claim one role, and picking silently is how a row gets written
	 * from a column the user did not mean. Duplicates are refused instead.
	 */
	AttrNumber	memberattno;
	AttrNumber	scoreattno;
	AttrNumber	valueattno;

	VfdwTableType tabletype;

	const char *keyprefix;
	const char *keyset;
	const char *singleton_key;
	const char *search_index;
	int			database;
	bool		legacy_value;
	bool		readonly;

	int			nfields;		/* VFDW_COL_FIELD columns */
} VfdwTableMap;

/*
 * Resolve and validate the shape of a foreign table.
 *
 * Raises if any attribute has no source, if required options conflict, or if
 * a column option is meaningless for the table type. Called at plan time so
 * a misdescribed table fails before a single command is sent.
 */
extern VfdwTableMap *vfdw_map_build(Relation rel, ForeignTable *table);
extern VfdwTableMap *vfdw_map_build_for_relid(Oid relid);

/*
 * Fill in everything a VfdwColumn derives from its PostgreSQL type.
 *
 * Exposed so that the diagnostic entry points describe a type exactly the way
 * a real table does. A probe that recomputed is_binary for itself would be a
 * second answer to the question the comment on that field says must have one.
 */
extern void vfdw_map_resolve_type(VfdwColumn *col, Oid typid, int32 typmod);

extern const char *vfdw_tabletype_name(VfdwTableType type);

/*
 * The CMD_* bitmask IsForeignRelUpdatable should report for a table, and the
 * reason for whatever it withholds.
 *
 * Deliberately does not build the map and cannot raise; see the definition.
 *
 * detail and hint are set to the reason a table accepts fewer commands than
 * all of them, and to NULL when it accepts all. Both may be NULL if the
 * caller only wants the mask. Returning the reason alongside the mask is not
 * a convenience: the refusal message used to carry one hardcoded explanation,
 * so a table blocked for having a search index was refused with a paragraph
 * about list row identity.
 */
extern int	vfdw_map_writability(Oid relid, const char **detail,
								 const char **hint);
extern int	vfdw_map_writability_for_relid(Oid relid);

#endif							/* VFDW_MAP_H */
